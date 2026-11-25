#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SMS_SEEN_CAP 512

typedef struct {
    uint64_t hashes[SMS_SEEN_CAP];
    unsigned count;
    unsigned next;
    uint32_t sequence;
    /* False once the journal file cannot be read or written: the in-memory ring is
     * then the only record, so a crash can make an already forwarded SMS look new.
     * Surfaced as `status.sms.journal_ok` instead of failing delivery silently. */
    bool journal_ok;
    char path[128];
} sms_seen_store_t;

bool sms_seen_init(sms_seen_store_t *store, const char *path);
bool sms_seen_contains(const sms_seen_store_t *store, uint64_t hash);
/* Returns true only after the fixed-slot journal record is synced to storage. */
bool sms_seen_commit(sms_seen_store_t *store, uint64_t hash);
typedef bool (*sms_seen_stage_fn)(void *context);

/* Three-way answer, because "the journal is unreadable" and "the journal says no"
 * have opposite consequences: the first must keep the retained copy, the second
 * may release it. */
typedef enum {
    SMS_SEEN_PENDING = 0, /* definitely not journalled yet */
    SMS_SEEN_ACKED,       /* definitely journalled */
    SMS_SEEN_UNKNOWN      /* journal unreadable, cannot decide */
} sms_seen_state_t;

sms_seen_state_t sms_seen_state(sms_seen_store_t *store, uint64_t hash);

/* Production delivery gate: send, journal the success, then (best effort) persist
 * the quote-reply mapping.  A failed map write no longer blocks the commit. */
bool sms_seen_deliver(sms_seen_store_t *store, uint64_t hash,
    sms_seen_stage_fn send_stage, sms_seen_stage_fn map_stage, void *context);
