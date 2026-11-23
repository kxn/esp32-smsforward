#include "sms_seen.h"

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
    if (!f) return errno == ENOENT;
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
    if (!f) return false;
    bool ok = fseek(f, (long)((seq % SMS_SEEN_CAP) * sizeof(r)), SEEK_SET) == 0 &&
              fwrite(&r, sizeof(r), 1, f) == 1 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) return false;
    s->hashes[s->next] = hash;
    if (s->count < SMS_SEEN_CAP) s->count++;
    s->next = (s->next + 1) % SMS_SEEN_CAP;
    s->sequence = seq;
    return true;
}

bool sms_seen_deliver(sms_seen_store_t *s, uint64_t hash,
    sms_seen_stage_fn send_stage, sms_seen_stage_fn map_stage, void *context)
{
    if (!s || sms_seen_contains(s, hash)) return s && sms_seen_contains(s, hash);
    if (!send_stage || !map_stage || !send_stage(context) || !map_stage(context)) return false;
    return sms_seen_commit(s, hash);
}
