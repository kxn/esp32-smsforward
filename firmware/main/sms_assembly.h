#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define SMS_ASSEMBLY_SLOTS 16
#define SMS_ASSEMBLY_PARTS 255
#define SMS_ASSEMBLY_MAX_BYTES 8192

typedef struct {
    char *text;
    struct sms_sim_ref_s *sim_refs;
    size_t index_count;
    size_t index_capacity;
} sms_assembly_part_t;

typedef struct sms_sim_ref_s { int index; uint64_t identity; } sms_sim_ref_t;

typedef struct {
    bool used;
    bool oversized;
    bool mms_notification;
    bool binary_payload_hex;
    bool timestamp_from_sequence_one;
    int slot;
    int reference;
    int reference_width;
    int total;
    int have;
    char sender[40];
    time_t timestamp;
    uint64_t deadline_ms;
    size_t text_bytes;
    sms_assembly_part_t *parts;
} sms_assembly_t;

typedef struct { sms_assembly_t slots[SMS_ASSEMBLY_SLOTS]; } sms_assembly_pool_t;

typedef enum {
    SMS_ASSEMBLY_INVALID = -3,
    SMS_ASSEMBLY_OVERSIZE = -2,
    SMS_ASSEMBLY_FULL = -1,
    SMS_ASSEMBLY_DUPLICATE = 0,
    SMS_ASSEMBLY_ADDED = 1,
    SMS_ASSEMBLY_COMPLETE = 2,
    SMS_ASSEMBLY_CONFLICT = 3
} sms_assembly_status_t;

void sms_assembly_init(sms_assembly_pool_t *pool);
void sms_assembly_destroy(sms_assembly_pool_t *pool);
sms_assembly_status_t sms_assembly_add(sms_assembly_pool_t *pool, int slot,
    const char *sender, int reference, int reference_width, int total, int sequence,
    time_t timestamp, bool mms_notification, bool binary_payload_hex,
    int sim_index, uint64_t identity, const char *text,
    uint64_t now_ms, unsigned timeout_ms, int *assembly_index);
/* Returns 1 when ready, 0 when incomplete/not timed out, negative on bounds error. */
int sms_assembly_build(const sms_assembly_t *assembly, bool timeout,
    char *out, size_t out_size, int *received, int *total);
void sms_assembly_clear(sms_assembly_t *assembly);
