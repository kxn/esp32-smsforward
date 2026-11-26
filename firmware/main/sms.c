#include "sms.h"
#include "pdu.h"
#include "cmd.h"
#include "feishu.h"
#include "health.h"
#include "wifi.h"
#include "sms_assembly.h"
#include "sms_seen.h"
#include "sms_text.h"
#include "sms_time_policy.h"
#include "sms_spool.h"
#include "peer_mark.h"
#include "peer_mark_core.h"
#include "cmd_policy.h"
#include "modem_timing.h"
#include "wap_mms.h"
#include "mms_inbox.h"
#include "mms_policy.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "sms";

static QueueHandle_t s_sms_q;
static modem_t *s_modems;
static SemaphoreHandle_t s_config_lock[MODEM_COUNT];
static bool s_slot_configured[MODEM_COUNT];

static EXT_RAM_BSS_ATTR sms_seen_store_t s_seen;
static SemaphoreHandle_t s_delivery_lock;

static int64_t sms_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static time_t modem_clock_utc(modem_t *m, time_t fallback)
{
    char resp[128] = {0};
    if (modem_cmd(m, "AT+CCLK?", resp, sizeof(resp), 800) != ESP_OK) return fallback;
    char *q = strchr(resp, '"');
    if (!q) return fallback;
    int yy, mo, dd, hh, mm, ss, zone;
    char sign;
    if (sscanf(q + 1, "%2d/%2d/%2d,%2d:%2d:%2d%c%2d",
               &yy, &mo, &dd, &hh, &mm, &ss, &sign, &zone) != 8 ||
        yy < 0 || yy > 99 || mo < 1 || mo > 12 || dd < 1 ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59 ||
        (sign != '+' && sign != '-') || zone < 0 || zone > 56) return fallback;
    static const uint8_t month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int year = 2000 + yy;
    int max_day = month_days[mo - 1];
    if (mo == 2 && year % 4 == 0) max_day = 29;
    if (dd > max_day) return fallback;
    int64_t local = sms_days_from_civil(year, (unsigned)mo, (unsigned)dd) * INT64_C(86400) +
                    hh * INT64_C(3600) + mm * INT64_C(60) + ss;
    int64_t offset = (int64_t)zone * 15 * 60;
    int64_t seconds = local + (sign == '+' ? -offset : offset);
    time_t result = (time_t)seconds;
    return seconds > 0 && (int64_t)result == seconds ? result : fallback;
}

static time_t sms_display_time(modem_t *m, time_t message_time, bool *synced)
{
    bool device_synced = time_synced();
    time_t now = time(NULL);
    time_t modem_now = (!message_time && !device_synced) ? modem_clock_utc(m, now) : 0;
    return sms_time_choose(message_time, device_synced, now, modem_now, synced);
}

static uint64_t sms_fingerprint(int slot, const char *peer, time_t ts, const char *text)
{
    uint64_t h = UINT64_C(14695981039346656037);
#define MIX_BYTE(b) do { h ^= (uint8_t)(b); h *= UINT64_C(1099511628211); } while (0)
    MIX_BYTE(slot);
    for (const unsigned char *p = (const unsigned char *)peer; *p; ++p) MIX_BYTE(*p);
    for (unsigned i = 0; i < sizeof(ts); ++i) MIX_BYTE(((uint64_t)ts >> (i * 8)) & 0xff);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) MIX_BYTE(*p);
#undef MIX_BYTE
    return h ? h : 1;
}

static bool seen_contains(uint64_t h)
{
    return sms_seen_contains(&s_seen, h);
}

typedef struct {
    const char *text;
    const char *record_id;
    int slot;
    const char *peer;
    char *message_id;
    size_t message_id_size;
} sms_delivery_context_t;

static bool delivery_send_stage(void *context)
{
    sms_delivery_context_t *c = context;
    return feishu_send_text_uuid(c->text, c->record_id,
                                 c->message_id, c->message_id_size) == ESP_OK;
}

static bool delivery_map_stage(void *context)
{
    sms_delivery_context_t *c = context;
    return map_note_persisted(c->message_id, c->slot, c->peer, c->record_id);
}

/* The SIM sweep is the durable retry queue.  While the upstream is down it finds
 * nothing to forward every time, so sweeping every 30 s only costs AT traffic and
 * power; idle sweeps back off to 5 minutes and any successful forward resets it. */
#define SMS_SWEEP_MIN_MS (30 * 1000)
#define SMS_SWEEP_MAX_MS (5 * 60 * 1000)
static uint32_t s_sweep_delay_ms = SMS_SWEEP_MIN_MS;
static volatile unsigned s_forwarded_since_sweep;

/* ---------------- 断网期间的待发送缓存 ---------------- */

/* Parks one notification for later delivery.  The text was built once, at receipt
 * time, so a replay after an outage still carries the original receive/ring time. */
static void spool_park(uint8_t kind, int slot, time_t ts, bool ts_valid,
                       uint64_t fingerprint, const char *peer, const char *record_id,
                       const char *text)
{
    if (!sms_spool_park(kind, slot, (int64_t)ts, ts_valid, fingerprint, peer, record_id, text)) {
        ESP_LOGE(TAG, "%s notification cannot be parked (queue full)",
                 kind == SMS_SPOOL_KIND_CALL ? "call" : "sms");
    }
}

/* Replay callback: runs on the spool task with the delivery lock held so it can
 * never race the SIM sweep for the same message. */
static bool spool_send_entry(const sms_spool_entry_t *e)
{
    if (!e) return false;
    if (s_delivery_lock) xSemaphoreTake(s_delivery_lock, portMAX_DELAY);
    bool ok = false;
    if (e->kind == SMS_SPOOL_KIND_SMS && seen_contains(e->fingerprint)) {
        /* The SIM sweep delivered it while we were parked. */
        ok = true;
        goto done;
    }
    if (!feishu_configured()) goto done;
    {
        char mid[64] = "";
        esp_err_t err = feishu_send_text_uuid(e->text, e->record_id[0] ? e->record_id : NULL,
                                              mid, sizeof(mid));
        if (err != ESP_OK) goto done;
        if (e->kind == SMS_SPOOL_KIND_SMS) {
            /* Stop the retained SIM copy from being forwarded a second time. */
            sms_seen_commit(&s_seen, e->fingerprint);
        }
        if (e->peer[0] && mid[0]) map_note_persisted(mid, e->slot, e->peer, e->record_id);
        __atomic_add_fetch(&s_forwarded_since_sweep, 1, __ATOMIC_RELAXED);
        ok = true;
    }
done:
    if (s_delivery_lock) xSemaphoreGive(s_delivery_lock);
    return ok;
}

typedef struct {
    int type;
    int slot;
    int idx;
    int capacity;
    int remaining;
    char peer[40];
    time_t call_time;
    uint64_t call_mono_ms;
    bool call_time_synced;
} sms_job_t;

enum { SMS_JOB_READ = 1, SMS_JOB_SCAN_START, SMS_JOB_SCAN_STEP, SMS_JOB_FLUSH, SMS_JOB_CALL };

/* ---------------- 闀跨煭淇℃嫾鎺?---------------- */

#define CONCAT_TIMEOUT_MS 60000
static EXT_RAM_BSS_ATTR sms_assembly_pool_t s_concat;
static SemaphoreHandle_t s_concat_lock;

static bool deliver(modem_t *m, const char *peer, bool sender_is_phone,
                    time_t ts, const char *text,
                    bool complete, int got, int total);
static bool deliver_mms_notice(modem_t *m, const char *peer, time_t ts, const char *hex, bool complete);

static bool read_sms_at(modem_t *m, int idx, sms_deliver_t *d)
{
    char cmd[32], resp[1600] = {0};
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idx);
    if (modem_cmd(m, cmd, resp, sizeof(resp), 2000) != ESP_OK) return false;
    char *save = NULL;
    char *line = strtok_r(resp, "\n", &save);
    while (line && strncmp(line, "+CMGR:", 6) != 0) line = strtok_r(NULL, "\n", &save);
    char *pdu_hex = line ? strtok_r(NULL, "\n", &save) : NULL;
    if (!pdu_hex) return false;
    uint8_t bin[512];
    size_t n = pdu_hex2bin(pdu_hex, bin, sizeof(bin));
    return n > 0 && pdu_decode_deliver(bin, n, d) == 0;
}

static uint64_t deliver_identity(int slot, const sms_deliver_t *d)
{
    return sms_fingerprint(slot, d->sender, d->scts, d->text);
}

static bool sim_identity_matches(modem_t *m, int idx, uint64_t expected)
{
    sms_deliver_t current;
    return read_sms_at(m, idx, &current) && deliver_identity(m->idx, &current) == expected;
}

static void delete_sim_entry(modem_t *m, int idx, uint64_t expected)
{
    if (!m || idx <= 0) {
        ESP_LOGW(TAG, "refusing to delete invalid SIM index %d", idx);
        return;
    }
    if (!sim_identity_matches(m, idx, expected)) {
        ESP_LOGW(TAG, "SIM SMS %d changed before delete; retained", idx);
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", idx);
    if (modem_cmd(m, cmd, NULL, 0, 5000) != ESP_OK)
        ESP_LOGW(TAG, "could not delete delivered SIM SMS %d", idx);
}

/* After this many consecutive push failures the upstream is clearly down: park new
 * events immediately instead of spending a 10 s HTTP timeout on each of them while
 * the SIM card fills up.  The spool task still probes for recovery. */
#define DELIVERY_STREAK_PARK 5


static bool feishu_path_down(void)
{
    return feishu_send_fail_streak() >= DELIVERY_STREAK_PARK;
}

/* A full SIM card is how SMS were lost during the 30 h outage: the card was the
 * only durable home and the network drops new messages once it is full.  The card
 * entry of a parked notification is pure duplication, but only once the spool holds
 * it in flash, so a nearly full card first forces one bulk persist.  The entries
 * themselves are released by the normal index sweep (see sms_process_index), which
 * already reads every index: no extra AT traffic, and progress is monotonic. */
/* Called when the SIM card is about to fill up: the pending events must reach
 * flash before their SIM copies may be released.  Persisting rewrites the whole
 * ring, so while an outage keeps producing events this would otherwise rewrite
 * megabytes on every sweep; the rate limit keeps the flash-wear cost bounded and
 * the next sweep retries. */
#define ENSURE_DURABLE_MIN_INTERVAL_MS (60 * 1000)

static void ensure_pending_durable(void)
{
    static int64_t last_try_ms;
    int64_t now = esp_timer_get_time() / 1000;
    if (last_try_ms && now - last_try_ms < ENSURE_DURABLE_MIN_INTERVAL_MS) return;
    if (sms_spool_pending() == 0) return;
    if (sms_spool_all_saved()) return;
    last_try_ms = now;
    if (!sms_spool_persist()) {
        ESP_LOGW(TAG, "the pending events are not durable yet; keeping the SIM copies");
    }
}

static void *sms_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static void concat_flush(int i, bool timeout)
{
    sms_assembly_t *a = &s_concat.slots[i];
    if (!a->used) return;
    char *merged = sms_alloc(SMS_ASSEMBLY_MAX_BYTES + 24);
    if (!merged) { ESP_LOGE(TAG, "concat buffer allocation failed"); return; }
    int got = 0, total = 0;
    int built = sms_assembly_build(a, timeout, merged, SMS_ASSEMBLY_MAX_BYTES + 24, &got, &total);
    if (built <= 0) {
        ESP_LOGW(TAG, "concat oversized/unbuildable; stored SIM entries retained");
        sms_assembly_clear(a);
        free(merged);
        return;
    }
    bool valid_text = true;
    if (a->binary_payload_hex && !a->mms_notification) {
        size_t bytes = 0, hex_len = strcspn(merged, "[");
        size_t cap = SMS_ASSEMBLY_MAX_BYTES + 24;
        valid_text = wap_mms_hex_decode(merged, hex_len, (uint8_t *)merged, cap, &bytes) == WAP_MMS_OK;
        int n = valid_text ? sms_text_display_8bit(merged, bytes, cap) : -1;
        valid_text = n >= 0;
        if (valid_text && got != total) snprintf(merged + n, cap - n, "[%d/%d]", got, total);
    }
    bool sent = a->mms_notification ?
        deliver_mms_notice(&s_modems[a->slot], a->sender, a->timestamp, merged, got == total) :
        valid_text && deliver(&s_modems[a->slot], a->sender, a->sender_is_phone, a->timestamp, merged,
                got == total, got, total);
    if (sent) {
        for (int p = 0; p < a->total; ++p) {
            sms_assembly_part_t *part = &a->parts[p];
            for (size_t j = 0; j < part->index_count; ++j) {
                delete_sim_entry(&s_modems[a->slot], part->sim_refs[j].index,
                                 part->sim_refs[j].identity);
            }
        }
    }
    sms_assembly_clear(a);
    free(merged);
}

static void concat_add(modem_t *m, int sim_index, const sms_deliver_t *d)
{
    uint64_t identity = deliver_identity(m->idx, d);
    xSemaphoreTake(s_concat_lock, portMAX_DELAY);
    int ai = -1;
    sms_assembly_status_t st = sms_assembly_add(&s_concat, m->idx, d->sender,
        d->concat_ref, d->concat_ref_width, d->concat_total, d->concat_seq,
        d->scts, d->mms_notification, d->binary_payload_hex, d->sender_is_phone,
        sim_index, identity, d->text,
        (uint64_t)(esp_timer_get_time() / 1000), CONCAT_TIMEOUT_MS, &ai);
    if (st == SMS_ASSEMBLY_COMPLETE) concat_flush(ai, false);
    else if (st < 0 || st == SMS_ASSEMBLY_CONFLICT)
        ESP_LOGW(TAG, "concat retained (status %d)", st);
    xSemaphoreGive(s_concat_lock);
}

static void concat_timer_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int64_t now = esp_timer_get_time() / 1000;
        xSemaphoreTake(s_concat_lock, portMAX_DELAY);
        for (int i = 0; i < SMS_ASSEMBLY_SLOTS; i++) {
            if (s_concat.slots[i].used && now >= s_concat.slots[i].deadline_ms) {
                sms_job_t job = { .type = SMS_JOB_FLUSH, .idx = i };
                xQueueSend(s_sms_q, &job, 0);
            }
        }
        xSemaphoreGive(s_concat_lock);
    }
}

/* ---------------- 鎶曢€?---------------- */

static bool deliver(modem_t *m, const char *peer, bool sender_is_phone,
                    time_t ts, const char *text,
                    bool complete, int got, int total)
{
    if (s_delivery_lock) xSemaphoreTake(s_delivery_lock, portMAX_DELAY);
    uint64_t fingerprint = sms_fingerprint(m->idx, peer, ts, text);
    if (seen_contains(fingerprint)) {
        ESP_LOGI(TAG, "duplicate SMS suppressed");
        if (s_delivery_lock) xSemaphoreGive(s_delivery_lock);
        return true;
    }
    ESP_LOGI(TAG, "%s SMS received (%s %d/%d)", modem_name(m),
             complete ? "complete" : "partial", got, total);
    health_sms_blink();

    size_t out_cap = SMS_ASSEMBLY_MAX_BYTES + 512;
    char *out = sms_alloc(out_cap);
    if (!out) {
        if (s_delivery_lock) xSemaphoreGive(s_delivery_lock);
        return false;
    }
    bool synced = false;
    time_t display_ts = sms_display_time(m, ts, &synced);
    int formatted = fmt_incoming(out, out_cap, m->idx, cmd_slot_name(m->idx),
                                 cmd_slot_number(m->idx), peer, sender_is_phone, display_ts, synced, text);
    if (formatted < 0 || (size_t)formatted >= out_cap) {
        ESP_LOGE(TAG, "formatted SMS exceeds delivery bound; SIM copy retained");
        free(out);
        if (s_delivery_lock) xSemaphoreGive(s_delivery_lock);
        return false;
    }
    char record_id[24];
    snprintf(record_id, sizeof(record_id), "sms_%016" PRIx64, fingerprint);
    if (!feishu_configured() || feishu_path_down()) {
        /* Park it with its original timestamp; the SIM copy stays as the reboot
         * safety net until the spool confirmed the delivery. */
        spool_park(SMS_SPOOL_KIND_SMS, m->idx, display_ts, synced, fingerprint, peer,
                   record_id, out);
        ESP_LOGW(TAG, "Feishu unavailable; SMS parked, SIM copy retained");
        free(out);
        if (s_delivery_lock) xSemaphoreGive(s_delivery_lock);
        return false;
    }
    char mid[64] = "";
    sms_delivery_context_t ctx = { .text = out, .record_id = record_id, .slot = m->idx,
                                   .peer = peer, .message_id = mid,
                                   .message_id_size = sizeof(mid) };
    bool accepted = sms_seen_deliver(&s_seen, fingerprint, delivery_send_stage,
                                    delivery_map_stage, &ctx);
    if (accepted) {
        __atomic_add_fetch(&s_forwarded_since_sweep, 1, __ATOMIC_RELAXED);
        ESP_LOGI(TAG, "forwarded, msg_id=%s", mid);
        free(out);
        if (s_delivery_lock) xSemaphoreGive(s_delivery_lock);
        return true;
    }
    /* Parked as well so the retry keeps the arrival order and survives a restart
     * even when the SIM card is full. */
    spool_park(SMS_SPOOL_KIND_SMS, m->idx, display_ts, synced, fingerprint, peer,
               record_id, out);
    ESP_LOGW(TAG, "forward/map/journal failed; parked, SIM copy retained");
    free(out);
    if (s_delivery_lock) xSemaphoreGive(s_delivery_lock);
    return false;
}

/* Completion is committed by the background worker; the ordinary SIM sweep can
 * then delete the original entries after rereading and checking each identity. */
static bool mms_commit(uint64_t key)
{
    xSemaphoreTake(s_delivery_lock, portMAX_DELAY);
    bool ok = sms_seen_commit(&s_seen, key);
    xSemaphoreGive(s_delivery_lock);
    return ok;
}

static bool deliver_mms_notice(modem_t *m, const char *peer, time_t ts, const char *hex, bool complete)
{
    if (!feishu_configured()) return false;
    wap_mms_info_t info = {0};
    size_t n = strlen(hex), bytes = 0;
    if (complete && n && !(n & 1)) {
        uint8_t *raw = sms_alloc(n / 2);
        if (!raw) return false;
        bool valid = wap_mms_hex_decode(hex, n, raw, n / 2, &bytes) == WAP_MMS_OK &&
                     wap_mms_parse_push(raw, bytes, &info) == WAP_MMS_OK;
        free(raw);
        if (!valid) memset(&info, 0, sizeof(info));
    }
    /* The SMS transport sender may be the WAP gateway, whereas From identifies
     * the actual originator. Accept only a phone address for reply routing. */
    char *suffix = strstr(info.sender, "/TYPE=PLMN");
    if (suffix && !suffix[10]) *suffix = 0;
    char dial[32], display[32];
    const char *region_peer = NULL;
    if (cmd_policy_dial_number(info.sender, dial, sizeof(dial), display, sizeof(display))) {
        peer = dial;
        region_peer = info.sender;
    } else if (info.message_type == 0x82) {
        /* A gateway number is not the author of this MMS. A non-dialable
         * mapping keeps the notification linkable without replying to it. */
        peer = "未知";
    }
    uint64_t fingerprint = sms_fingerprint(m->idx, peer, ts, hex) ^ UINT64_C(0xd16c0176892baf4d);
    uint64_t complete_key = fingerprint ^ UINT64_C(0xab69001c4f7b10e4);
    xSemaphoreTake(s_delivery_lock, portMAX_DELAY);
    if (seen_contains(complete_key)) {
        xSemaphoreGive(s_delivery_lock);
        return true;
    }
    char out[384];
    bool synced = false;
    time_t display_ts = sms_display_time(m, ts, &synced);
    int formatted = cmd_policy_format_message_notice(out, sizeof(out), "彩信", peer, region_peer,
                    cmd_slot_number(m->idx), display_ts, synced, NULL);
    if (formatted < 0 || (size_t)formatted >= sizeof(out)) {
        if (s_delivery_lock) xSemaphoreGive(s_delivery_lock);
        return false;
    }
    char record_id[24];
    snprintf(record_id, sizeof(record_id), "mms_%016" PRIx64, fingerprint);
    char mid[64] = "";
    sms_delivery_context_t ctx = { .text = out, .record_id = record_id, .slot = m->idx,
                                   .peer = peer, .message_id = mid,
                                   .message_id_size = sizeof(mid) };
    bool ok = sms_seen_deliver(&s_seen, fingerprint, delivery_send_stage,
                               delivery_map_stage, &ctx);
    xSemaphoreGive(s_delivery_lock);
    if (!ok || !info.content_location[0] ||
        (info.has_message_size && info.message_size_bytes > MMS_BODY_MAX)) return ok;
    if (info.has_expiry) {
        uint64_t until = info.expiry_seconds;
        if (info.expiry_relative) {
            if (ts <= 0 || until > UINT64_MAX - (uint64_t)ts) return true;
            until += (uint64_t)ts;
        }
        if (!time_synced()) return false;
        if ((uint64_t)time(NULL) >= until) return true;
    }
    if (!mid[0] && !map_find_notice(record_id, mid, sizeof(mid))) return false;
    mms_inbox_schedule(m, info.content_location, mid, record_id, complete_key, mms_commit);
    return false;
}

/* ---------------- 读取 ---------------- */

typedef struct {
    char chunk[280];
    uint8_t pdu[700];
    char hex[1600];
    char response[512];
} sms_send_scratch_t;

int sms_send(modem_t *m, const char *dest, const char *text, char *err, size_t errlen)
{
    int total_units = sms_text_ucs2_units(text);
    if (total_units < 0) {
        snprintf(err, errlen, "invalid utf8");
        return -1;
    }
    if (total_units == 0) {
        snprintf(err, errlen, "empty");
        return -1;
    }

    int cap = total_units <= 70 ? 70 : 67;
    int segments = sms_text_segment_count(text);
    if (segments < 0) {
        snprintf(err, errlen, "invalid utf8");
        return -1;
    }
    if (segments < 1 || segments > 8) {
        snprintf(err, errlen, "too long");
        return -2;
    }

    sms_send_scratch_t *scratch = malloc(sizeof(*scratch));
    if (!scratch) {
        snprintf(err, errlen, "out of memory");
        return -3;
    }

    const char *p = text;
    int ref = (int)(esp_random() & 0xFF);

    for (int seg = 1; seg <= segments; seg++) {
        int got = sms_text_next_chunk(p, scratch->chunk, sizeof(scratch->chunk), cap);
        if (got <= 0) {
            snprintf(err, errlen, "invalid utf8");
            free(scratch);
            return -3;
        }

        size_t off = 0;
        int tpdu = pdu_encode_submit(m->sca, dest, scratch->chunk, ref, segments, seg,
                                     scratch->pdu, sizeof(scratch->pdu), &off);
        if (tpdu <= 0) {
            snprintf(err, errlen, "encode fail");
            free(scratch);
            return -3;
        }
        scratch->response[0] = '\0';
        pdu_bin2hex(scratch->pdu, (size_t)tpdu + off, scratch->hex);

        esp_err_t e = modem_sms_pdu(m, scratch->hex, tpdu, scratch->response,
                                    sizeof(scratch->response), AT_SMS_SUBMIT_MS);
        if (e != ESP_OK) {
            int cme=cmd_policy_cme_code(scratch->response);
            snprintf(err,errlen,"modem=%d;cme=%d;done=%d;total=%d",
                     (int)e,cme,seg-1,segments);
            free(scratch);
            if(seg>1) return SMS_SEND_ERR_PARTIAL;
            return e==ESP_FAIL?SMS_SEND_ERR_MODEM:(int)e;
        }
        p += strlen(scratch->chunk);
    }
    if (*p != '\0') {
        snprintf(err, errlen, "incomplete segmentation");
        free(scratch);
        return -3;
    }
    free(scratch);
    return 0;
}

static int sms_process_index(modem_t *m, int idx)
{
    sms_deliver_t d;
    if (!read_sms_at(m, idx, &d)) {
        /* Empty SIM positions return CMS ERROR and are expected during an index sweep. */
        return -1;
    }

    bool ok = true;
    if (d.has_udh && d.concat_total > 1) {
        concat_add(m, idx, &d);
        return 0; /* assembly owns this SIM entry until delivery succeeds */
    } else {
        ok = d.mms_notification ? deliver_mms_notice(m, d.sender, d.scts, d.text, true) :
             deliver(m, d.sender, d.sender_is_phone, d.scts, d.text, true, 1, 1);
    }

    uint64_t identity = deliver_identity(m->idx, &d);
    if (ok) {
        delete_sim_entry(m, idx, identity);
    } else if (sms_spool_holds_durably(identity)) {
        /* Parked and saved: this card entry is a duplicate, and keeping it would
         * fill the card and make the network drop new SMS. */
        delete_sim_entry(m, idx, identity);
        ESP_LOGW(TAG, "sms %d is safe in the spool; SIM slot released", idx);
    } else {
        ESP_LOGW(TAG, "keep sms %d for retry", idx);
    }
    return 0;
}

int sms_read_index(modem_t *m, int idx)
{
    if (!m || !s_sms_q || idx <= 0) return -1;
    sms_job_t job = { .type = SMS_JOB_READ, .slot = m->idx, .idx = idx };
    return xQueueSend(s_sms_q, &job, 0) == pdTRUE ? 0 : -1;
}

/* ---------------- 鏉ョ數 ---------------- */

static esp_timer_handle_t s_ring_timer[MODEM_COUNT];
/* Ring state is written by the modem URC task (RING/+CLIP) and consumed by
 * ring_task, so every access goes through s_ring_mux; the AT work in ring_task
 * happens outside the critical section. */
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ring_active[MODEM_COUNT];
static char s_ring_peer[MODEM_COUNT][40];
static uint64_t s_ring_start_ms[MODEM_COUNT];
static time_t s_ring_start_epoch[MODEM_COUNT];
static bool s_ring_time_synced[MODEM_COUNT];
static QueueHandle_t s_ring_q;

static void ring_timeout(void *arg)
{
    int slot = (int)(intptr_t)arg;
    if (slot >= 0 && slot < MODEM_COUNT && s_ring_q) xQueueSend(s_ring_q, &slot, 0);
}

static void ring_task(void *arg)
{
    (void)arg;
    int slot;
    while (xQueueReceive(s_ring_q, &slot, portMAX_DELAY) == pdTRUE) {
        if (slot < 0 || slot >= MODEM_COUNT) continue;
        static const char unknown[] = "\xE6\x9C\xAA\xE7\x9F\xA5";
        char peer[40];
        uint64_t start_ms;
        time_t start_epoch;
        bool time_ok;
        bool active;
        taskENTER_CRITICAL(&s_ring_mux);
        active = s_ring_active[slot];
        if (active) {
            strlcpy(peer, s_ring_peer[slot][0] ? s_ring_peer[slot] : unknown, sizeof(peer));
            s_ring_peer[slot][0] = '\0';
            s_ring_active[slot] = false;
            start_ms = s_ring_start_ms[slot];
            start_epoch = s_ring_start_epoch[slot];
            time_ok = s_ring_time_synced[slot];
        }
        taskEXIT_CRITICAL(&s_ring_mux);
        if (!active) continue;
        if (!strcmp(peer, unknown)) {
            char resp[512] = {0};
            if (modem_cmd(&s_modems[slot], "AT+CLCC", resp, sizeof(resp), 400) == ESP_OK) {
                char *save = NULL;
                for (char *line = strtok_r(resp, "\n", &save); line;
                     line = strtok_r(NULL, "\n", &save)) {
                    int id, dir, stat, mode, mpty;
                    if (sscanf(line, "+CLCC: %d,%d,%d,%d,%d", &id, &dir, &stat, &mode, &mpty) != 5 ||
                        dir != 1 || (stat != 4 && stat != 5)) continue;
                    char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                    if (q1 && q2 && q2 > q1 + 1 && (size_t)(q2 - q1 - 1) < sizeof(peer)) {
                        size_t n = (size_t)(q2 - q1 - 1);
                        memcpy(peer, q1 + 1, n);
                        peer[n] = '\0';
                        break;
                    }
                }
            }
        }
        bool hung = modem_cmd(&s_modems[slot], "ATH", NULL, 0, AT_HANGUP_MS) == ESP_OK;
        if (!hung) hung = modem_cmd(&s_modems[slot], "AT+CHUP", NULL, 0, 800) == ESP_OK;
        if (hung) {
            ESP_LOGI(TAG, "call handled on slot %d after %llums", slot + 1,
                     (unsigned long long)(esp_timer_get_time() / 1000 - start_ms));
        } else {
            /* Report it anyway: a missed call has no durable home, so refusing to
             * notify because the hangup was not acknowledged loses a real event,
             * while a redundant reject is harmless. */
            ESP_LOGW(TAG, "call hangup unconfirmed on slot %d; reporting it anyway", slot + 1);
        }
        sms_job_t job = { .type = SMS_JOB_CALL, .slot = slot,
                          .call_time = start_epoch,
                          .call_mono_ms = start_ms,
                          .call_time_synced = time_ok };
        strlcpy(job.peer, peer, sizeof(job.peer));
        if (xQueueSend(s_sms_q, &job, 0) != pdTRUE) ESP_LOGW(TAG, "call queue full on slot %d", slot + 1);
    }
}

static void call_notice(int slot, const char *peer, time_t ring_time,
                        uint64_t ring_mono_ms, bool ring_time_synced)
{
    uint64_t now_mono=(uint64_t)(esp_timer_get_time()/1000);
    uint64_t elapsed_s=now_mono>=ring_mono_ms?(now_mono-ring_mono_ms)/1000:0;
    bool synced=ring_time_synced || time_synced();
    time_t ts;
    if(ring_time_synced) ts=ring_time;
    else if(time_synced()) ts=time(NULL)-(time_t)elapsed_s;
    else ts=modem_clock_utc(&s_modems[slot],time(NULL))-(time_t)elapsed_s;
    static const char unknown[]="\xE6\x9C\xAA\xE7\x9F\xA5";
    char dial[40],display[40];
    const char *caller=(peer && peer[0])?peer:unknown;
    bool valid_peer=peer && cmd_policy_dial_number(peer,dial,sizeof(dial),display,sizeof(display));
    /* A missed call has no durable home (unlike SMS, which the SIM card keeps), so
     * an unreachable Feishu used to drop it for good.  Park it with its ring time
     * and replay once the link is back. */
    bool will_send = feishu_configured() && !feishu_path_down();
    const char *region = phone_origin_label(caller);
    const char *label = region;
    char merged[96];
    /* The online mark is opt-in: with no service configured this branch costs one
     * config read and nothing else. */
    if (will_send && valid_peer && peer_mark_enabled()) {
        /* Only callers we cannot name locally are worth a network round trip, and
         * the lookup gives up after its own budget, so a slow service costs a
         * second or two at worst. */
        const char *local_label = NULL;
        if (phone_origin_lookup(caller, &local_label) != PHONE_ORIGIN_SERVICE) {
            const char *mark = peer_mark_lookup(peer);
            if (mark && mark[0]) {
                peer_mark_join_label(merged, sizeof(merged), region, mark);
                label = merged;
            }
        }
    }
    char text[384];
    int n=cmd_policy_format_call_notice(text,sizeof(text),slot,cmd_slot_name(slot),
                                        cmd_slot_number(slot),caller,label,ts,synced);
    if(n<0 || (size_t)n>=sizeof(text)) return;
    uint64_t fingerprint = sms_fingerprint(slot, caller, ts, text);
    char record_id[24];
    snprintf(record_id, sizeof(record_id), "call_%016" PRIx64, fingerprint);
    if (!will_send) {
        spool_park(SMS_SPOOL_KIND_CALL, slot, ts, synced, fingerprint, peer, record_id, text);
        return;
    }
    char mid[64] = "";
    if (feishu_send_text(text, mid, sizeof(mid)) == ESP_OK) {
        if (valid_peer) map_note(mid,slot,peer);
        return;
    }
    spool_park(SMS_SPOOL_KIND_CALL, slot, ts, synced, fingerprint, peer, record_id, text);
    ESP_LOGW(TAG, "call notice parked for replay");
}

void sms_urc(modem_t *m, const char *line)
{
    if (strncmp(line, "+CMTI:", 6) == 0) {
        int idx = -1;
        char *comma = strrchr(line, ',');
        if (comma) idx = atoi(comma + 1);
        if (idx >= 0) {
            sms_job_t job = { .type = SMS_JOB_READ, .slot = m->idx, .idx = idx };
            if (xQueueSend(s_sms_q, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "%s sms queue full", modem_name(m));
            }
        }
        return;
    }

    if (strncmp(line, "RING", 4) == 0) {
        int slot = m->idx;
        if (slot >= 0 && slot < MODEM_COUNT) {
            bool started = false;
            taskENTER_CRITICAL(&s_ring_mux);
            if (!s_ring_active[slot]) {
                s_ring_active[slot] = true;
                s_ring_start_ms[slot] = esp_timer_get_time() / 1000;
                s_ring_start_epoch[slot] = time(NULL);
                s_ring_time_synced[slot] = time_synced();
                started = true;
            }
            taskEXIT_CRITICAL(&s_ring_mux);
            /* esp_timer_start_once may allocate, so it stays outside the critical section. */
            if (started && s_ring_timer[slot]) esp_timer_start_once(s_ring_timer[slot], 800000);
        }
        return;
    }

    if (strncmp(line, "+CLIP:", 6) == 0) {
        char *q1 = strchr(line, '"');
        char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        int slot = m->idx;
        if (slot >= 0 && slot < MODEM_COUNT && q1 && q2 &&
            (size_t)(q2 - q1 - 1) < sizeof(s_ring_peer[slot])) {
            size_t n = (size_t)(q2 - q1 - 1);
            taskENTER_CRITICAL(&s_ring_mux);
            memcpy(s_ring_peer[slot], q1 + 1, n);
            s_ring_peer[slot][n] = '\0';
            taskEXIT_CRITICAL(&s_ring_mux);
        }
        return;
    }

    /* URCs may contain SMS payloads or caller numbers; never log raw lines. */
}

void sms_run(void)
{
    vTaskPrioritySet(NULL, 9);
    sms_job_t job;
    bool scanning[MODEM_COUNT] = {0};
    TickType_t watermark_log_at = 0;
    while (1) {
        if (xQueueReceive(s_sms_q, &job, portMAX_DELAY) == pdTRUE) {
            if (job.type == SMS_JOB_READ && job.slot >= 0 && job.slot < MODEM_COUNT) {
                if ((sms_modem_is_configured(job.slot) ||
                     sms_modem_configure(job.slot) == ESP_OK))
                    sms_process_index(&s_modems[job.slot], job.idx);
            } else if (job.type == SMS_JOB_SCAN_START && job.slot >= 0 && job.slot < MODEM_COUNT) {
                char resp[192] = {0};
                if (!scanning[job.slot] &&
                    (sms_modem_is_configured(job.slot) ||
                     sms_modem_configure(job.slot) == ESP_OK) &&
                    modem_cmd(&s_modems[job.slot], "AT+CPMS?", resp, sizeof(resp), 3000) == ESP_OK) {
                    const char *p = strstr(resp, "+CPMS:");
                    int used, capacity;
                    if (p && sscanf(p, "+CPMS: \"%*[^\"]\",%d,%d", &used, &capacity) == 2 &&
                        used > 0 && used <= capacity && capacity <= 512) {
                        /* Nearly full and holding unsaved events?  Make them durable
                         * now so the sweep below may release their SIM copies. */
                        if (used >= capacity - 1) ensure_pending_durable();
                        sms_job_t next = { .type = SMS_JOB_SCAN_STEP, .slot = job.slot,
                                           .idx = 1, .capacity = capacity, .remaining = used };
                        scanning[job.slot] = xQueueSend(s_sms_q, &next, 0) == pdTRUE;
                    }
                }
            } else if (job.type == SMS_JOB_SCAN_STEP && job.slot >= 0 &&
                       job.slot < MODEM_COUNT && job.idx > 0 && job.idx <= job.capacity) {
                if (sms_process_index(&s_modems[job.slot], job.idx) == 0) job.remaining--;
                job.idx++;
                scanning[job.slot] = job.remaining > 0 && job.idx <= job.capacity &&
                                     xQueueSend(s_sms_q, &job, 0) == pdTRUE;
            } else if (job.type == SMS_JOB_FLUSH && job.idx >= 0 &&
                       job.idx < SMS_ASSEMBLY_SLOTS) {
                xSemaphoreTake(s_concat_lock, portMAX_DELAY);
                sms_assembly_t *a = &s_concat.slots[job.idx];
                if (a->used && esp_timer_get_time() / 1000 >= (int64_t)a->deadline_ms)
                    concat_flush(job.idx, true);
                xSemaphoreGive(s_concat_lock);
            } else if (job.type == SMS_JOB_CALL && job.slot >= 0 && job.slot < MODEM_COUNT) {
                call_notice(job.slot,job.peer,job.call_time,job.call_mono_ms,
                            job.call_time_synced);
            }
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - watermark_log_at) >= pdMS_TO_TICKS(30000)) {
            UBaseType_t low = uxTaskGetStackHighWaterMark(NULL);
            if (low < 2048) ESP_LOGW(TAG, "SMS worker stack low-water: %u bytes", (unsigned)low);
            watermark_log_at = now;
        }
    }
}

void sms_retry_pending(void)
{
    /* Replay whatever the outage parked, in arrival order. */
    sms_spool_notify();
    if (!s_modems || !s_sms_q || !feishu_configured()) return;
    for (int i = 0; i < MODEM_COUNT; i++) {
        sms_job_t job = { .type = SMS_JOB_SCAN_START, .slot = i };
        xQueueSend(s_sms_q, &job, 0);
    }
}

static void sms_retry_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_sweep_delay_ms));
        /* SIM storage is the durable queue; retry even after transient network
         * errors.  An idle sweep means the outage has not cleared yet. */
        unsigned forwarded = __atomic_exchange_n(&s_forwarded_since_sweep, 0, __ATOMIC_RELAXED);
        if (forwarded) {
            s_sweep_delay_ms = SMS_SWEEP_MIN_MS;
        } else if (s_sweep_delay_ms < SMS_SWEEP_MAX_MS) {
            s_sweep_delay_ms *= 2;
            if (s_sweep_delay_ms > SMS_SWEEP_MAX_MS) s_sweep_delay_ms = SMS_SWEEP_MAX_MS;
        }
        sms_retry_pending();
    }
}

uint32_t sms_sweep_delay_ms(void)
{
    return s_sweep_delay_ms;
}

bool sms_journal_ok(void)
{
    return s_seen.journal_ok;
}

esp_err_t sms_modem_configure(int slot)
{
    if (!s_modems || slot < 0 || slot >= MODEM_COUNT || !s_config_lock[slot])
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_config_lock[slot], portMAX_DELAY);
    s_slot_configured[slot] = false;
    modem_t *m = &s_modems[slot];

    /* SMSC is useful for outbound delivery but not required for receiving.
     * Keep the last known value if this optional query fails. */
    char resp[256] = {0};
    if (modem_cmd(m, "AT+CSCA?", resp, sizeof(resp), AT_SMSC_MS) == ESP_OK) {
        char *q = strchr(resp, '"');
        char *e = q ? strchr(q + 1, '"') : NULL;
        if (q && e && (size_t)(e - q - 1) < sizeof(m->sca)) {
            memcpy(m->sca, q + 1, (size_t)(e - q - 1));
            m->sca[e - q - 1] = '\0';
        }
    }

    static const char *const setup[] = {
        "AT+CMGF=0",
        "AT+CPMS=\"SM\",\"SM\",\"SM\"",
        "AT+CNMI=2,1,0,0,0",
        "AT+CLIP=1",
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        esp_err_t err = modem_cmd(m, setup[i], NULL, 0, AT_SETUP_MS);
        if (err != ESP_OK) {
            ok = false;
            ESP_LOGW(TAG, "%s SMS setup step %u failed (%d)",
                     modem_name(m), (unsigned)(i + 1), (int)err);
        }
    }
    s_slot_configured[slot] = ok;
    xSemaphoreGive(s_config_lock[slot]);
    return ok ? ESP_OK : ESP_FAIL;
}

bool sms_modem_is_configured(int slot)
{
    if (slot < 0 || slot >= MODEM_COUNT || !s_config_lock[slot]) return false;
    xSemaphoreTake(s_config_lock[slot], portMAX_DELAY);
    bool configured = s_slot_configured[slot];
    xSemaphoreGive(s_config_lock[slot]);
    return configured;
}

esp_err_t sms_init(modem_t *modems)
{
    s_modems = modems;
    for (int i = 0; i < MODEM_COUNT; i++) {
        if (!s_config_lock[i]) s_config_lock[i] = xSemaphoreCreateMutex();
        s_slot_configured[i] = false;
        if (!s_config_lock[i]) return ESP_ERR_NO_MEM;
    }
    s_sms_q = xQueueCreate(16, sizeof(sms_job_t));
    s_concat_lock = xSemaphoreCreateMutex();
    s_delivery_lock = xSemaphoreCreateMutex();
    s_ring_q = xQueueCreate(8, sizeof(int));
    if (!s_sms_q || !s_concat_lock || !s_delivery_lock || !s_ring_q)
        return ESP_ERR_NO_MEM;
    sms_assembly_init(&s_concat);
    if (!sms_seen_init(&s_seen, "/data/sms_seen.dat")) return ESP_FAIL;
    sms_spool_set_send_fn(spool_send_entry);
    if (xTaskCreate(ring_task, "ring", 12288, NULL, 9, NULL) != pdPASS ||
        xTaskCreate(concat_timer_task, "concat", 3072, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(sms_retry_task, "sms_retry", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "required SMS worker creation failed");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < MODEM_COUNT; i++) {
        esp_timer_create_args_t targs = { .callback = ring_timeout, .arg = (void *)(intptr_t)i,
                                          .name = i == 0 ? "ring0" : "ring1" };
        esp_err_t err = esp_timer_create(&targs, &s_ring_timer[i]);
        if (err != ESP_OK) return err;
    }

    for (int i = 0; i < MODEM_COUNT; i++) {
        modems[i].urc_cb = sms_urc;
        if (sms_modem_configure(i) == ESP_OK)
            ESP_LOGI(TAG, "%s SMS configured", modem_name(&modems[i]));
        else
            ESP_LOGW(TAG, "%s SMS configuration pending retry", modem_name(&modems[i]));
    }

    /* Process SIM-stored SMS after the owner task starts. */
    sms_retry_pending();
    return ESP_OK;
}
