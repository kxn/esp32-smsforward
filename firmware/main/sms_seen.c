#include "sms_seen.h"

#include "esp_log.h"

static const char *TAG = "sms_seen";

#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SMS_SEEN_MAGIC UINT32_C(0x534d5331)
#pragma pack(push, 1)
typedef struct { uint32_t magic, seq; uint64_t hash; uint32_t crc; } sms_seen_record_t;
#pragma pack(pop)

static uint32_t record_crc(const sms_seen_record_t *r)
{
    uint32_t c = UINT32_C(2166136261);
    const uint8_t *p = (const uint8_t *)r;
    for (size_t i = 0; i < offsetof(sms_seen_record_t, crc); ++i) {
        c ^= p[i];
        c *= UINT32_C(16777619);
    }
    return c;
}

bool sms_seen_init(sms_seen_store_t *s, const char *path)
{
    if (!s || !path || strlen(path) >= sizeof(s->path)) return false;
    memset(s, 0, sizeof(*s));
    strcpy(s->path, path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        s->journal_ok = errno == ENOENT;
        return s->journal_ok;
    }
    s->journal_ok = true; /* the file is readable; replay below tells the rest */
    bool have_newest = false;
    uint32_t newest = 0;
    for (unsigned i = 0; i < SMS_SEEN_CAP; ++i) {
        sms_seen_record_t r;
        if (fseek(f, (long)(i * sizeof(r)), SEEK_SET) != 0 || fread(&r, sizeof(r), 1, f) != 1)
            continue;
        if (r.magic == SMS_SEEN_MAGIC && r.crc == record_crc(&r) &&
            (!have_newest || (int32_t)(r.seq - newest) > 0)) {
            newest = r.seq;
            have_newest = true;
        }
    }
    if (!have_newest) { fclose(f); return true; }
    s->sequence = newest;
    for (unsigned age = SMS_SEEN_CAP; age > 0; --age) {
        uint32_t seq = newest - (age - 1);
        unsigned i = seq % SMS_SEEN_CAP;
        sms_seen_record_t r;
        if (fseek(f, (long)(i * sizeof(r)), SEEK_SET) == 0 &&
            fread(&r, sizeof(r), 1, f) == 1 && r.seq == seq &&
            r.magic == SMS_SEEN_MAGIC && r.crc == record_crc(&r)) {
            s->hashes[s->count++] = r.hash;
            s->next = s->count % SMS_SEEN_CAP;
        }
    }
    fclose(f);
    return true;
}

bool sms_seen_contains(const sms_seen_store_t *s, uint64_t hash)
{
    if (!s) return false;
    for (unsigned i = 0; i < s->count; ++i) if (s->hashes[i] == hash) return true;
    return false;
}

bool sms_seen_commit(sms_seen_store_t *s, uint64_t hash)
{
    if (!s || !s->path[0]) return false;
    uint32_t seq = s->sequence + 1;
    sms_seen_record_t r = { .magic = SMS_SEEN_MAGIC, .seq = seq, .hash = hash };
    r.crc = record_crc(&r);
    FILE *f = fopen(s->path, "r+b");
    if (!f) f = fopen(s->path, "w+b");
    if (!f) {
        s->journal_ok = false;
        return false;
    }
    bool ok = fseek(f, (long)((seq % SMS_SEEN_CAP) * sizeof(r)), SEEK_SET) == 0 &&
              fwrite(&r, sizeof(r), 1, f) == 1 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        s->journal_ok = false;
        return false;
    }
    s->journal_ok = true;
    s->hashes[s->next] = hash;
    if (s->count < SMS_SEEN_CAP) s->count++;
    s->next = (s->next + 1) % SMS_SEEN_CAP;
    s->sequence = seq;
    return true;
}

sms_seen_state_t sms_seen_state(sms_seen_store_t *s, uint64_t hash)
{
    if (!s) return SMS_SEEN_UNKNOWN;
    if (sms_seen_contains(s, hash)) return SMS_SEEN_ACKED;
    return s->journal_ok ? SMS_SEEN_PENDING : SMS_SEEN_UNKNOWN;
}

bool sms_seen_deliver(sms_seen_store_t *s, uint64_t hash,
    sms_seen_stage_fn send_stage, sms_seen_stage_fn map_stage, void *context)
{
    /* A degraded journal (state UNKNOWN) still forwards: for a notification device,
     * a possible duplicate is the lesser evil compared to a stall, and
     * `status.sms.journal_ok` makes the condition visible. */
    if (sms_seen_state(s, hash) == SMS_SEEN_ACKED) return true;
    if (!send_stage) return false;
    if (!send_stage(context)) return false;
    /* Journal the success before the quote-reply bookkeeping.  Neither write may
     * veto the delivery that already happened: the map is a convenience (it lets a
     * reply reach the original thread) and the journal only dedups a retry of a
     * SIM copy that should have been deleted.  Reporting failure here kept the SIM
     * copy around and re-pushed the same message on every sweep, which is exactly
     * the duplicate flood this journal was supposed to prevent. */
    if (!sms_seen_commit(s, hash)) {
        ESP_LOGW(TAG, "delivery journal write failed; the message stays delivered "
                      "(possible duplicate only if power is lost before the SIM "
                      "copy is deleted)");
    }
    if (map_stage) (void)map_stage(context);
    return true;
}
