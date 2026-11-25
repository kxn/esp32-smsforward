#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "sms_spool_core.h"

/* Runtime glue for the pending-delivery spool (see sms_spool_core.h for the data
 * model).  The store lives in PSRAM and is written to flash only when a restart
 * would otherwise lose events: SMS keep a copy on the SIM card, call notices do
 * not, so the spill file is mostly about calls and about freeing SIM space.
 *
 * The owner of the SMS/Feishu semantics supplies the send callback; this module
 * only decides when to replay and when to persist. */

typedef bool (*sms_spool_send_fn)(const sms_spool_entry_t *entry);

void sms_spool_set_send_fn(sms_spool_send_fn fn);

/* Allocates the store in PSRAM, restores the spill file and starts the replay
 * task.  Safe to call once from app_main. */
esp_err_t sms_spool_start(void);

/* Wake the replay task (link came back, new entry queued, ...). */
void sms_spool_notify(void);

/* Queue one event for later delivery.  Returns false when the store is full (the
 * caller must then keep its own copy or drop the event).  The notification text is
 * already formatted by the caller, so the entry is built in PSRAM here instead of
 * the caller needing an ~8.4 KB stack copy.
 * Re-parking an event that is already queued keeps its position and its durability
 * (a retry must not make an already persisted event look unsaved again, which
 * would rewrite the whole spill file on every sweep). */
bool sms_spool_park(uint8_t kind, int slot, int64_t received_unix, bool ts_valid,
                    uint64_t fingerprint, const char *peer, const char *record_id,
                    const char *text);

bool sms_spool_contains(uint64_t fingerprint);
/* True when this event is queued *and* a copy is known to be in flash, i.e. the
 * caller may drop its own copy (the retained SIM entry). */
bool sms_spool_holds_durably(uint64_t fingerprint);
size_t sms_spool_pending(void);
/* True when every queued event already has a copy in flash. */
bool sms_spool_all_saved(void);

/* Write every pending entry to flash.  Returns false when the write failed or the
 * blob could not be serialised; entries stay in RAM either way.  Returns true when
 * nothing needed writing because every entry already had a copy in flash. */
bool sms_spool_persist(void);

/* Remove the spill file (after a successful full replay). */
void sms_spool_clear_persisted(void);

void sms_spool_status_json(cJSON *parent);
