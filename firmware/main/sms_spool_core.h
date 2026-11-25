#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure, host-testable pending-delivery spool.
 *
 * Purpose: while Feishu/Wi-Fi is unreachable the device must not lose incoming
 * SMS or call notices, and it must not let the SIM card fill up either.  Every
 * event that cannot be forwarded is parked here (kept in RAM by the caller) and
 * replayed in arrival order once the link is healthy again.  The entry text is
 * formatted at receipt time, so a delayed replay still shows the original
 * receive/ring time.
 *
 * This module owns no memory of its own beyond the caller-provided store and
 * performs no I/O: the ESP-IDF glue in sms_spool.c decides where the store lives
 * (PSRAM) and when a copy is persisted to flash. */

#define SMS_SPOOL_KIND_SMS 0
#define SMS_SPOOL_KIND_CALL 1

/* Must hold a fully assembled multipart SMS (SMS_ASSEMBLY_MAX_BYTES) plus the
 * notification header. */
#define SMS_SPOOL_TEXT_MAX 8448
#define SMS_SPOOL_PEER_MAX 48
#define SMS_SPOOL_ID_MAX 48
/* How many events the spool holds while the upstream is unreachable.  Each slot
 * is a fixed SMS_SPOOL_TEXT_MAX text plus ids (~8.6 KB), so the store lives in
 * PSRAM: 256 slots cost ~2.1 MB of the 7.8 MB the chip has.  The bound is not
 * memory but the spill file: persist() rewrites the whole ring as one blob, and
 * the blob has to fit the 4 MB storage partition together with a copy of itself
 * during the write (~2.2 MB worst case here).  Raising it further needs a
 * bounded/delta spill format, not more PSRAM. */
#define SMS_SPOOL_CAPACITY 256

typedef struct {
    uint8_t kind;   /* SMS_SPOOL_KIND_* */
    uint8_t slot;   /* 0 or 1 */
    uint8_t ts_valid; /* received_unix is trustworthy */
    uint8_t durable;  /* a copy exists in flash (spill file) */
    int64_t received_unix; /* original receive (SMS) or ring (call) time */
    uint64_t fingerprint;  /* dedup key, same value used by sms_seen */
    char peer[SMS_SPOOL_PEER_MAX];      /* sender / caller used for quote map */
    char record_id[SMS_SPOOL_ID_MAX];   /* journal id of the notification */
    char msg_id[SMS_SPOOL_ID_MAX];      /* Feishu message id it was sent as */
    uint16_t text_len;
    char text[SMS_SPOOL_TEXT_MAX + 1];
} sms_spool_entry_t;

typedef struct {
    sms_spool_entry_t entry[SMS_SPOOL_CAPACITY];
    unsigned count;
    uint32_t added;    /* statistics */
    uint32_t dropped;  /* events discarded because the store was full */
} sms_spool_store_t;

void sms_spool_init(sms_spool_store_t *s);
unsigned sms_spool_count(const sms_spool_store_t *s);
bool sms_spool_is_full(const sms_spool_store_t *s);

/* index 0 is the oldest pending event. */
const sms_spool_entry_t *sms_spool_at(const sms_spool_store_t *s, unsigned index);
const sms_spool_entry_t *sms_spool_find(const sms_spool_store_t *s, uint64_t fingerprint);

/* Append at the tail, preserving arrival order.  Returns NULL when the store is
 * full.  When the fingerprint is already present nothing is appended: the
 * existing entry is refreshed (text/durability) and returned. */
sms_spool_entry_t *sms_spool_add(sms_spool_store_t *s, const sms_spool_entry_t *e);

bool sms_spool_mark_durable(sms_spool_store_t *s, uint64_t fingerprint, bool durable);

/* Decide whether the pending entries should be flushed to flash now.  This is the
 * only place that knows the wear-levelling compromise: write on the first pending
 * event, when enough events piled up, or once the period elapsed -- never per
 * event.  A `min_entries` of 0 disables the size trigger. */
bool sms_spool_should_persist(unsigned pending, unsigned min_entries,
                              int64_t last_persist_ms, int64_t now_ms,
                              int64_t period_ms);
bool sms_spool_all_durable(const sms_spool_store_t *s);

/* Drop a delivered (or superseded) entry.  Returns true when something was
 * removed. */
bool sms_spool_remove(sms_spool_store_t *s, uint64_t fingerprint);

/* Upper bound of the serialised size for `count` entries.  The wire-format owner
 * publishes it so callers never hard-code a buffer size that silently excludes a
 * full ring (a too-small cap there used to make persist() fail for good). */
size_t sms_spool_encoded_max(unsigned count);

/* Serialise every entry, oldest first.  Returns the byte count written, or 0
 * when `cap` is too small. */
size_t sms_spool_encode(const sms_spool_store_t *s, uint8_t *out, size_t cap);

/* Restore entries from a blob produced by sms_spool_encode.  Appends to `s`
 * (caller normally calls sms_spool_init first) and returns the number restored.
 * On a malformed/truncated blob nothing is appended and *corrupt is set. */
unsigned sms_spool_decode(sms_spool_store_t *s, const uint8_t *buf, size_t len, bool *corrupt);
