#include "sms_assembly.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sms_assembly_init(sms_assembly_pool_t *pool)
{
    if (pool) memset(pool, 0, sizeof(*pool));
}

void sms_assembly_clear(sms_assembly_t *a)
{
    if (!a) return;
    for (int i = 0; i < a->total; ++i) {
        free(a->parts[i].text);
        free(a->parts[i].sim_refs);
    }
    free(a->parts);
    memset(a, 0, sizeof(*a));
}

void sms_assembly_destroy(sms_assembly_pool_t *pool)
{
    if (!pool) return;
    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; ++i) sms_assembly_clear(&pool->slots[i]);
}

static bool add_index(sms_assembly_part_t *p, int idx, uint64_t identity)
{
    for (size_t i = 0; i < p->index_count; ++i)
        if (p->sim_refs[i].index == idx) return true;
    if (p->index_count == p->index_capacity) {
        size_t cap = p->index_capacity ? p->index_capacity * 2 : 2;
        sms_sim_ref_t *v = realloc(p->sim_refs, cap * sizeof(*v));
        if (!v) return false;
        p->sim_refs = v;
        p->index_capacity = cap;
    }
    p->sim_refs[p->index_count++] = (sms_sim_ref_t){ .index = idx, .identity = identity };
    return true;
}

sms_assembly_status_t sms_assembly_add(sms_assembly_pool_t *pool, int slot,
    const char *sender, int reference, int reference_width, int total, int sequence,
    time_t timestamp, bool mms_notification, bool binary_payload_hex,
    int sim_index, uint64_t identity, const char *text,
    uint64_t now_ms, unsigned timeout_ms, int *assembly_index)
{
    if (!pool || !sender || !text || slot < 0 || slot > 1 || !sender[0] ||
        total < 2 || total > SMS_ASSEMBLY_PARTS || sequence < 1 || sequence > total ||
        (reference_width != 8 && reference_width != 16) || sim_index < 1)
        return SMS_ASSEMBLY_INVALID;

    int found = -1, free_slot = -1;
    for (int i = 0; i < SMS_ASSEMBLY_SLOTS; ++i) {
        sms_assembly_t *a = &pool->slots[i];
        if (!a->used) { if (free_slot < 0) free_slot = i; continue; }
        if (a->slot == slot && a->reference == reference &&
            a->reference_width == reference_width && a->total == total &&
            strcmp(a->sender, sender) == 0) {
            if (now_ms >= a->deadline_ms) return SMS_ASSEMBLY_FULL;
            found = i;
            break;
        }
    }
    if (found < 0) {
        if (free_slot < 0) return SMS_ASSEMBLY_FULL;
        found = free_slot;
        sms_assembly_t *a = &pool->slots[found];
        memset(a, 0, sizeof(*a));
        a->used = true;
        a->slot = slot;
        a->reference = reference;
        a->reference_width = reference_width;
        a->total = total;
        a->parts = calloc((size_t)total, sizeof(*a->parts));
        if (!a->parts) { memset(a, 0, sizeof(*a)); return SMS_ASSEMBLY_FULL; }
        a->timestamp = timestamp;
        a->deadline_ms = now_ms + timeout_ms;
        snprintf(a->sender, sizeof(a->sender), "%s", sender);
    }

    sms_assembly_t *a = &pool->slots[found];
    if (assembly_index) *assembly_index = found;
    if (mms_notification) a->mms_notification = true;
    if (binary_payload_hex) a->binary_payload_hex = true;
    if (sequence == 1 && timestamp) {
        a->timestamp = timestamp;
        a->timestamp_from_sequence_one = true;
    } else if (!a->timestamp_from_sequence_one && timestamp &&
               (!a->timestamp || timestamp < a->timestamp)) {
        a->timestamp = timestamp;
    }
    sms_assembly_part_t *part = &a->parts[sequence - 1];
    if (part->text) {
        if (strcmp(part->text, text) != 0) return SMS_ASSEMBLY_CONFLICT;
        if (!add_index(part, sim_index, identity)) return SMS_ASSEMBLY_FULL;
        return SMS_ASSEMBLY_DUPLICATE;
    }

    size_t n = strlen(text);
    if (n > SMS_ASSEMBLY_MAX_BYTES - a->text_bytes) {
        a->oversized = true;
        return SMS_ASSEMBLY_OVERSIZE;
    }
    char *copy = strdup(text);
    if (!copy || !add_index(part, sim_index, identity)) {
        free(copy);
        return SMS_ASSEMBLY_FULL;
    }
    part->text = copy;
    a->text_bytes += n;
    a->have++;
    return a->have == a->total ? SMS_ASSEMBLY_COMPLETE : SMS_ASSEMBLY_ADDED;
}

int sms_assembly_build(const sms_assembly_t *a, bool timeout,
    char *out, size_t out_size, int *received, int *total)
{
    if (!a || !a->used || !out || !out_size || a->oversized) return -1;
    if (!timeout && a->have != a->total) return 0;
    size_t used = 0;
    int got = 0;
    out[0] = '\0';
    for (int i = 0; i < a->total; ++i) {
        const char *s = a->parts[i].text;
        if (!s) continue;
        size_t n = strlen(s);
        if (n >= out_size - used) return -1;
        memcpy(out + used, s, n);
        used += n;
        out[used] = '\0';
        got++;
    }
    if (timeout && got != a->total) {
        char suffix[16];
        int n = snprintf(suffix, sizeof(suffix), "[%d/%d]", got, a->total);
        if (n < 0 || (size_t)n >= out_size - used) return -1;
        memcpy(out + used, suffix, (size_t)n + 1);
        used += (size_t)n;
    }
    if (received) *received = got;
    if (total) *total = a->total;
    return 1;
}
