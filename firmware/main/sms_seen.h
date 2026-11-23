#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SMS_SEEN_CAP 512

typedef struct {
    uint64_t hashes[SMS_SEEN_CAP];
    unsigned count;
    unsigned next;
    uint32_t sequence;
    char path[128];
} sms_seen_store_t;

bool sms_seen_init(sms_seen_store_t *store, const char *path);
bool sms_seen_contains(const sms_seen_store_t *store, uint64_t hash);
/* Returns true only after the fixed-slot journal record is synced to storage. */
bool sms_seen_commit(sms_seen_store_t *store, uint64_t hash);
typedef bool (*sms_seen_stage_fn)(void *context);
/* Production delivery gate: send, persist mapping, then durable dedup commit. */
bool sms_seen_deliver(sms_seen_store_t *store, uint64_t hash,
    sms_seen_stage_fn send_stage, sms_seen_stage_fn map_stage, void *context);
