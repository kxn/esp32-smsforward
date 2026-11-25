#include "sms_spool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "feishu.h"
#include "net_health.h"
#include "storage.h"
#include "storage.h"
#include "wifi.h"

static const char *TAG = "spool";

#define SPOOL_PATH "/data/spool.dat"
#define SPOOL_TMP_PATH "/data/spool.tmp"
/* The replay callback performs a full HTTPS request (TLS handshake included), so
 * this task needs as much stack as the SMS worker used to need for the same job. */
#define SPOOL_TASK_STACK 12288
#define SPOOL_TASK_PRIO 3
#define SPOOL_RETRY_MIN_MS (30 * 1000)
#define SPOOL_RETRY_MAX_MS (5 * 60 * 1000)
/* Persist at most this often while events are still pending: a broken upstream
 * must not turn into a flash wearing contest. */
#define SPOOL_PERSIST_PERIOD_MS (10 * 60 * 1000)
#define SPOOL_PERSIST_MIN_ENTRIES 8
#define SPOOL_ENCODE_START_CAP (8 * 1024)

static sms_spool_store_t *s_store;
static SemaphoreHandle_t s_lock;
/* Serialises the flash side: the SMS worker may ask for a spill while the replay
 * task is persisting on its own schedule. */
static SemaphoreHandle_t s_io_lock;
static TaskHandle_t s_task;
static sms_spool_send_fn s_send_fn;
static int64_t s_last_persist_ms;
static int64_t s_retry_backoff_ms = SPOOL_RETRY_MIN_MS;
static volatile bool s_persist_failed;

static void store_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void store_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

/* Any allocation large enough to hold the widest possible blob is transient: use
 * PSRAM first so the internal heap stays available for TLS and HTTP. */
static void *spool_alloc(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(len);
    return p;
}

static size_t encode_pending(uint8_t **out)
{
    /* The bound comes from the format owner; growing by powers of two and stopping
     * there would leave the exact worst-case size untried.  A full ring of long
     * SMS needs more than 512 KiB, so that last attempt is the one that matters. */
    size_t max_cap = sms_spool_encoded_max(sms_spool_count(s_store));
    size_t cap = SPOOL_ENCODE_START_CAP;
    while (1) {
        if (cap > max_cap) cap = max_cap;
        uint8_t *buf = spool_alloc(cap);
        if (!buf) return 0;
        size_t n = sms_spool_encode(s_store, buf, cap);
        if (n > 0) {
            *out = buf;
            return n;
        }
        free(buf);
        if (cap >= max_cap) return 0;
        cap *= 2;
    }
}

static void clear_persisted_locked(void);

bool sms_spool_persist(void)
{
    if (!s_store) return false;
    if (s_io_lock) xSemaphoreTake(s_io_lock, portMAX_DELAY);
    uint8_t *blob = NULL;
    size_t len = 0;

    store_lock();
    if (sms_spool_count(s_store) == 0) {
        store_unlock();
        clear_persisted_locked();
        if (s_io_lock) xSemaphoreGive(s_io_lock);
        return false;
    }
    if (sms_spool_all_durable(s_store)) {
        /* Everything is already on flash: nothing to rewrite. */
        store_unlock();
        if (s_io_lock) xSemaphoreGive(s_io_lock);
        return true;
    }
    /* Remember exactly which events this blob contains: something may be parked
     * while the write below runs, and marking those later events durable would let
     * the SIM copy of a message that is not on flash be released. */
    unsigned snapshot = sms_spool_count(s_store);
    uint64_t *snap_fp = spool_alloc(snapshot * sizeof(*snap_fp));
    if (snap_fp) {
        for (unsigned i = 0; i < snapshot; ++i) {
            const sms_spool_entry_t *e = sms_spool_at(s_store, i);
            snap_fp[i] = e ? e->fingerprint : 0;
        }
    } else {
        snapshot = 0; /* no snapshot: fall back to marking everything below */
    }
    len = encode_pending(&blob);
    store_unlock();
    if (len == 0 || !blob) {
        free(blob);
        free(snap_fp);
        if (s_io_lock) xSemaphoreGive(s_io_lock);
        ESP_LOGE(TAG, "cannot serialise %u pending event(s)", (unsigned)sms_spool_pending());
        return false;
    }

    /* Writing the new blob next to the old one is the safe order (a power cut
     * leaves the previous copy intact), but it needs room for both.  The storage
     * partition is 4 MB, so a nearly full ring can exceed that: in that case the
     * old copy goes first and a power cut inside this window costs the persisted
     * events (the SMS themselves still sit on the SIM card). */
    struct stat old_st;
    bool room_for_both = false;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(STORAGE_MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK) {
        unsigned long long old_bytes = 0;
        if (stat(SPOOL_PATH, &old_st) == 0 && old_st.st_size > 0) {
            old_bytes = (unsigned long long)old_st.st_size;
        }
        room_for_both = free_bytes >= (uint64_t)len + old_bytes;
    }
    if (!room_for_both && remove(SPOOL_PATH) == 0) {
        ESP_LOGW(TAG, "spill file too large to keep both copies; rewriting in place");
    }

    FILE *f = fopen(SPOOL_TMP_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s (%d)", SPOOL_TMP_PATH, errno);
        free(blob);
        free(snap_fp);
        if (s_io_lock) xSemaphoreGive(s_io_lock);
        return false;
    }
    bool ok = fwrite(blob, 1, len, f) == len;
    if (ok) ok = fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    free(blob);
    if (!ok) {
        free(snap_fp);
        remove(SPOOL_TMP_PATH);
        ESP_LOGE(TAG, "spool write failed");
        if (s_io_lock) xSemaphoreGive(s_io_lock);
        return false;
    }
    /* Rename keeps the previous copy valid if power is lost mid-write. */
    remove(SPOOL_PATH);
    if (rename(SPOOL_TMP_PATH, SPOOL_PATH) != 0) {
        free(snap_fp);
        ESP_LOGE(TAG, "spool rename failed (%d)", errno);
        remove(SPOOL_TMP_PATH);
        if (s_io_lock) xSemaphoreGive(s_io_lock);
        return false;
    }

    store_lock();
    unsigned marked = 0;
    for (unsigned i = 0; i < snapshot; ++i) {
        if (snap_fp[i] && sms_spool_mark_durable(s_store, snap_fp[i], true)) marked++;
    }
    if (snapshot == 0) {
        for (unsigned i = 0; i < sms_spool_count(s_store); ++i) {
            const sms_spool_entry_t *e = sms_spool_at(s_store, i);
            if (e && !e->durable && sms_spool_mark_durable(s_store, e->fingerprint, true)) marked++;
        }
    }
    store_unlock();
    free(snap_fp);
    if (s_io_lock) xSemaphoreGive(s_io_lock);
    s_last_persist_ms = esp_timer_get_time() / 1000;
    s_persist_failed = false;
    ESP_LOGI(TAG, "persisted %u bytes, %u new event(s) made durable", (unsigned)len, marked);
    return true;
}

static void clear_persisted_locked(void)
{
    remove(SPOOL_TMP_PATH);
    remove(SPOOL_PATH);
}

void sms_spool_clear_persisted(void)
{
    if (s_io_lock) xSemaphoreTake(s_io_lock, portMAX_DELAY);
    clear_persisted_locked();
    if (s_io_lock) xSemaphoreGive(s_io_lock);
}

static bool load_persisted(void)
{
    struct stat st;
    if (stat(SPOOL_PATH, &st) != 0 || st.st_size <= 0) return false;
    if ((size_t)st.st_size > sms_spool_encoded_max((unsigned)SMS_SPOOL_CAPACITY)) {
        ESP_LOGW(TAG, "spill file too large (%ld), ignoring", (long)st.st_size);
        remove(SPOOL_PATH);
        return false;
    }
    FILE *f = fopen(SPOOL_PATH, "rb");
    if (!f) return false;
    uint8_t *buf = spool_alloc((size_t)st.st_size);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    bool corrupt = false;
    unsigned restored = 0;
    store_lock();
    restored = sms_spool_decode(s_store, buf, got, &corrupt);
    for (unsigned i = 0; i < sms_spool_count(s_store); ++i) {
        const sms_spool_entry_t *e = sms_spool_at(s_store, i);
        if (e) sms_spool_mark_durable(s_store, e->fingerprint, true);
    }
    store_unlock();
    free(buf);
    if (corrupt) {
        ESP_LOGW(TAG, "spill file corrupt, discarded");
        remove(SPOOL_PATH);
        return false;
    }
    if (restored) {
        ESP_LOGI(TAG, "restored %u pending event(s) from flash", restored);
    }
    return restored > 0;
}

static void replay_locked_entries(void)
{
    int sent = 0;
    while (1) {
        const sms_spool_entry_t *e;
        store_lock();
        e = sms_spool_at(s_store, 0);
        if (!e) {
            store_unlock();
            break;
        }
        /* The send callback may take seconds (HTTP/TLS), so it gets its own copy.
         * The copy lives in PSRAM: one entry is ~8.4 KB, far too much for a task
         * stack that also has to survive a TLS handshake. */
        sms_spool_entry_t *copy = spool_alloc(sizeof(*copy));
        if (!copy) {
            store_unlock();
            break;
        }
        *copy = *e;
        uint64_t fingerprint = copy->fingerprint;
        const sms_spool_entry_t *live = sms_spool_find(s_store, fingerprint);
        bool was_durable = live && live->durable;
        /* While the send is in flight the event is no longer guaranteed to be
         * replayed: mark it in memory only.  If the process dies mid-send the
         * flash copy is still there and the replay uses the same record_id, which
         * Feishu dedups by uuid. */
        sms_spool_mark_durable(s_store, fingerprint, false);
        store_unlock();
        if (!s_send_fn) {
            store_lock();
            sms_spool_mark_durable(s_store, fingerprint, was_durable);
            store_unlock();
            free(copy);
            break;
        }
        bool ok = s_send_fn(copy);
        free(copy);
        if (!ok) {
            store_lock();
            sms_spool_mark_durable(s_store, fingerprint, was_durable);
            store_unlock();
            break;
        }
        store_lock();
        sms_spool_remove(s_store, fingerprint);
        store_unlock();
        sent++;
        if (!feishu_configured()) break;
        /* Let the SMS worker (which shares the delivery lock) run between two sends;
         * a single tick is enough for its higher priority and keeps a full-ring
         * replay under a second of added latency.  Nothing to yield for once the
         * queue is empty. */
        if (sms_spool_pending() > 0) vTaskDelay(1);
    }
    if (sent) {
        ESP_LOGI(TAG, "replayed %d pending event(s)", sent);
    }
}

static void spool_task(void *arg)
{
    (void)arg;
    int64_t next_due_ms = 0;
    while (1) {
        int64_t now = esp_timer_get_time() / 1000;
        bool pending;
        bool all_durable;
        store_lock();
        pending = sms_spool_count(s_store) > 0;
        all_durable = !pending || sms_spool_all_durable(s_store);
        store_unlock();

        /* Replay only when the upstream is actually reachable: trying while the
         * internet is down costs a 10 s HTTP timeout per event and tells us
         * nothing that the supervisor's probe has not already told us. */
        if (pending && now >= next_due_ms && net_health_internet_ok() && feishu_configured()) {
            size_t before;
            store_lock();
            before = sms_spool_count(s_store);
            store_unlock();
            replay_locked_entries();
            store_lock();
            bool progressed = sms_spool_count(s_store) < before;
            store_unlock();
            if (!progressed) {
                s_retry_backoff_ms = s_retry_backoff_ms * 2;
                if (s_retry_backoff_ms > SPOOL_RETRY_MAX_MS) s_retry_backoff_ms = SPOOL_RETRY_MAX_MS;
            } else {
                s_retry_backoff_ms = SPOOL_RETRY_MIN_MS;
            }
            store_lock();
            all_durable = sms_spool_all_durable(s_store);
            pending = sms_spool_count(s_store) > 0;
            store_unlock();
            next_due_ms = esp_timer_get_time() / 1000 + s_retry_backoff_ms;
        }

        if (pending && !all_durable) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            size_t count;
            store_lock();
            count = sms_spool_count(s_store);
            store_unlock();
            if (sms_spool_should_persist((unsigned)count, SPOOL_PERSIST_MIN_ENTRIES,
                                         s_last_persist_ms, now_ms,
                                         SPOOL_PERSIST_PERIOD_MS)) {
                if (!sms_spool_persist()) {
                    s_persist_failed = true;
                    /* Back off so a full filesystem cannot spin the flash. */
                    s_last_persist_ms = now_ms;
                }
            }
        }
        if (!pending) {
            sms_spool_clear_persisted();
            /* A drained queue must not inherit the outage backoff, or the next
             * event would wait out a stale multi-minute delay. */
            s_retry_backoff_ms = SPOOL_RETRY_MIN_MS;
        }
        uint32_t wait_ms = 5000;
        if (pending) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (next_due_ms > now_ms) {
                int64_t delta = next_due_ms - now_ms;
                wait_ms = delta > 5000 ? 5000 : (uint32_t)delta;
            } else {
                wait_ms = 1000;
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t sms_spool_start(void)
{
    if (s_store) return ESP_OK;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_io_lock) {
        s_io_lock = xSemaphoreCreateMutex();
        if (!s_io_lock) return ESP_ERR_NO_MEM;
    }
    s_store = heap_caps_malloc(sizeof(sms_spool_store_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_store) s_store = malloc(sizeof(sms_spool_store_t));
    if (!s_store) {
        ESP_LOGE(TAG, "cannot allocate the pending-delivery spool");
        return ESP_ERR_NO_MEM;
    }
    sms_spool_init(s_store);
    if (!storage_ready()) {
        ESP_LOGW(TAG, "/data unavailable, pending events stay in RAM only");
    } else {
        load_persisted();
    }
    if (!xTaskCreate(spool_task, "spool", SPOOL_TASK_STACK, NULL, SPOOL_TASK_PRIO, &s_task)) {
        ESP_LOGE(TAG, "cannot start the spool task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void sms_spool_set_send_fn(sms_spool_send_fn fn)
{
    s_send_fn = fn;
}

void sms_spool_notify(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

/* Trim to a UTF-8 code point boundary so a parked notification never ends in the
 * middle of a multi-byte sequence (Feishu renders that as a stray byte). */
static size_t utf8_trim(const char *text, size_t len)
{
    while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) --len;
    return len;
}

bool sms_spool_park(uint8_t kind, int slot, int64_t received_unix, bool ts_valid,
                    uint64_t fingerprint, const char *peer, const char *record_id,
                    const char *text)
{
    if (!s_store || !text || !text[0]) return false;
    size_t len = strlen(text);
    if (len > SMS_SPOOL_TEXT_MAX) len = utf8_trim(text, SMS_SPOOL_TEXT_MAX);
    /* One entry is ~8.4 KB: build it in PSRAM, never on the caller's stack. */
    sms_spool_entry_t *e = spool_alloc(sizeof(*e));
    if (!e) {
        ESP_LOGE(TAG, "cannot allocate a pending event");
        return false;
    }
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->slot = (uint8_t)slot;
    e->ts_valid = ts_valid ? 1 : 0;
    e->received_unix = received_unix;
    e->fingerprint = fingerprint;
    if (peer) strlcpy(e->peer, peer, sizeof(e->peer));
    if (record_id) strlcpy(e->record_id, record_id, sizeof(e->record_id));
    memcpy(e->text, text, len);
    e->text_len = (uint16_t)len;

    store_lock();
    const sms_spool_entry_t *queued = sms_spool_find(s_store, fingerprint);
    if (queued) e->durable = queued->durable;
    bool stored = sms_spool_add(s_store, e) != NULL;
    size_t count = sms_spool_count(s_store);
    store_unlock();
    free(e);

    if (!stored) {
        ESP_LOGE(TAG, "pending queue full, event dropped");
        return false;
    }
    ESP_LOGW(TAG, "parked event for later delivery (%u pending)", (unsigned)count);
    sms_spool_notify();
    return true;
}

bool sms_spool_contains(uint64_t fingerprint)
{
    if (!s_store) return false;
    store_lock();
    bool found = sms_spool_find(s_store, fingerprint) != NULL;
    store_unlock();
    return found;
}

bool sms_spool_holds_durably(uint64_t fingerprint)
{
    if (!s_store) return false;
    store_lock();
    const sms_spool_entry_t *e = sms_spool_find(s_store, fingerprint);
    bool held = e && e->durable;
    store_unlock();
    return held;
}

bool sms_spool_all_saved(void)
{
    if (!s_store) return true;
    store_lock();
    bool saved = sms_spool_all_durable(s_store);
    store_unlock();
    return saved;
}

size_t sms_spool_pending(void)
{
    if (!s_store) return 0;
    store_lock();
    size_t n = sms_spool_count(s_store);
    store_unlock();
    return n;
}

void sms_spool_status_json(cJSON *parent)
{
    if (!parent || !s_store) return;
    store_lock();
    unsigned count = sms_spool_count(s_store);
    unsigned added = s_store->added;
    unsigned dropped = s_store->dropped;
    unsigned calls = 0;
    for (unsigned i = 0; i < count; ++i) {
        const sms_spool_entry_t *e = sms_spool_at(s_store, i);
        if (e && e->kind == SMS_SPOOL_KIND_CALL) calls++;
    }
    bool all_durable = !count || sms_spool_all_durable(s_store);
    store_unlock();
    cJSON_AddNumberToObject(parent, "pending", (double)count);
    cJSON_AddNumberToObject(parent, "pending_calls", (double)calls);
    cJSON_AddNumberToObject(parent, "added", (double)added);
    cJSON_AddNumberToObject(parent, "dropped", (double)dropped);
    cJSON_AddBoolToObject(parent, "durable", all_durable);
    cJSON_AddBoolToObject(parent, "persist_failed", s_persist_failed);
    cJSON_AddNumberToObject(parent, "capacity", (double)SMS_SPOOL_CAPACITY);
    /* Headroom of the spill file lives on the same 4 MB partition: report it so a
     * failing persist can be told apart from "the disk is full". */
    uint64_t storage_total = 0;
    uint64_t storage_free = 0;
    if (esp_vfs_fat_info(STORAGE_MOUNT_POINT, &storage_total, &storage_free) == ESP_OK) {
        cJSON_AddNumberToObject(parent, "storage_free_bytes", (double)storage_free);
    }
}
