#include "health.h"
#include "board.h"
#include "sms.h"
#include "cfg.h"
#include "feishu.h"
#include "firmware_update.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "health";

/* 原厂固件策略：5 分钟 AT 探活，2s 超时，连续 3 次无响应 -> GPIO 复位。
 * AT 只能证明 UART 活着；这里同时查 CPMS/CNMI，才能发现"AT 正常但收不到
 * 短信"的假死。短信路径连续 3 次超时同样复位。 */
#define PROBE_INTERVAL_MS     (5 * 60 * 1000)
#define PROBE_RETRY_MS        (30 * 1000)
#define PROBE_AFTER_RESET_MS  (20 * 1000)
#define PROBE_TIMEOUT_MS      2000
#define PROBE_SMS_TIMEOUT_MS  3000
#define PROBE_THRESHOLD       3
#define MAINT_RETRY_MS        (10 * 60 * 1000)
#define MAINT_STAGGER_MS      (5 * 60 * 1000)

static modem_t *s_modems;
static TaskHandle_t s_health_task;
static modem_health_status_t s_status[2];
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t s_manual_reset_mask;
static volatile bool s_probe_now;
static int64_t s_next_probe;
static int64_t s_next_maint[2];
static uint8_t s_maint_hours;

static volatile int s_blink_sms;
static volatile int s_blink_err;

void health_sms_blink(void) { s_blink_sms = 1; }
void health_error_blink(void) { s_blink_err = 2; }

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void notify_health(void)
{
    if (s_health_task) xTaskNotifyGive(s_health_task);
}

void health_get_status(int slot, modem_health_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= MODEM_COUNT) return;
    taskENTER_CRITICAL(&s_status_mux);
    *out = s_status[slot];
    taskEXIT_CRITICAL(&s_status_mux);
}

void health_request_probe(void)
{
    s_probe_now = true;
    notify_health();
}

bool health_request_reset(int slot)
{
    if (slot < -1 || slot >= MODEM_COUNT || !s_health_task) return false;
    if (sms_call_active()) return false;
    uint8_t mask = slot < 0 ? 0x03 : (uint8_t)(1u << slot);
    taskENTER_CRITICAL(&s_status_mux);
    s_manual_reset_mask |= mask;
    taskEXIT_CRITICAL(&s_status_mux);
    notify_health();
    return true;
}

static bool parse_cpms(const char *resp, int *used, int *capacity)
{
    const char *p = resp ? strstr(resp, "+CPMS:") : NULL;
    return p && sscanf(p, "+CPMS: \"%*[^\"]\",%d,%d", used, capacity) == 2;
}

static bool parse_cnmi(const char *resp, int *mode, int *bm)
{
    const char *p = resp ? strstr(resp, "+CNMI:") : NULL;
    return p && sscanf(p, "+CNMI: %d,%d", mode, bm) == 2;
}

/* Returns false when the hardware reset could not acquire the modem lock. */
static bool reset_modem(int slot, modem_reset_reason_t reason)
{
    modem_t *m = &s_modems[slot];
    esp_err_t err = modem_reset_hw(m);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s reset skipped (%d)", modem_name(m), (int)err);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_err_t cfg_err = sms_modem_configure(slot);
    if (cfg_err == ESP_OK)
        ESP_LOGI(TAG, "%s reconfigured after reset", modem_name(m));
    else
        ESP_LOGW(TAG, "%s SMS reconfiguration pending retry (%d)", modem_name(m), (int)cfg_err);

    int64_t now = now_ms();
    taskENTER_CRITICAL(&s_status_mux);
    s_status[slot].at_ok = false;
    s_status[slot].cpms_ok = false;
    s_status[slot].cnmi_ok = false;
    s_status[slot].at_fail_streak = 0;
    s_status[slot].sms_fail_streak = 0;
    s_status[slot].last_probe_ms = now;
    s_status[slot].last_reset_ms = now;
    s_status[slot].last_reset_reason = (uint8_t)reason;
    s_status[slot].resets++;
    taskEXIT_CRITICAL(&s_status_mux);
    /* 手动/故障复位也顺延定时维护，避免同一张卡刚复位又被维护任务复位。 */
    if (s_maint_hours)
        s_next_maint[slot] = now + (int64_t)s_maint_hours * 60 * 60 * 1000;
    /* Let the module finish booting before the next AT probe. */
    s_next_probe = now + PROBE_AFTER_RESET_MS;
    return true;
}

static void probe_slot(int slot)
{
    modem_t *m = &s_modems[slot];
    char resp[256] = {0};
    bool at_ok = false, cpms_ok = false, cnmi_ok = false;
    bool at_alive = false, sms_dead = false;

    esp_err_t at = modem_cmd(m, "AT", NULL, 0, PROBE_TIMEOUT_MS);
    if (at == MODEM_ERR_BUSY) return; /* another AT transaction owns the UART */
    at_alive = at == ESP_OK ||
               (at != ESP_ERR_TIMEOUT && at != MODEM_ERR_DESYNC);
    at_ok = at == ESP_OK;
    if (!at_alive) {
        ESP_LOGW(TAG, "%s no response (%d)", modem_name(m), (int)at);
    } else {
        if (!at_ok) ESP_LOGW(TAG, "%s AT returned error %d", modem_name(m), (int)at);

        esp_err_t e = modem_cmd(m, "AT+CPMS?", resp, sizeof(resp), PROBE_SMS_TIMEOUT_MS);
        if (e == ESP_OK) {
            cpms_ok = true;
            int used = 0, capacity = 0;
            if (parse_cpms(resp, &used, &capacity))
                ESP_LOGI(TAG, "%s CPMS used=%d cap=%d", modem_name(m), used, capacity);
            else
                ESP_LOGW(TAG, "%s CPMS response not understood", modem_name(m));
        } else {
            if (e == ESP_ERR_TIMEOUT || e == MODEM_ERR_DESYNC) sms_dead = true;
            ESP_LOGW(TAG, "%s CPMS probe failed (%d)", modem_name(m), (int)e);
        }

        memset(resp, 0, sizeof(resp));
        e = modem_cmd(m, "AT+CNMI?", resp, sizeof(resp), PROBE_SMS_TIMEOUT_MS);
        if (e == ESP_OK) {
            cnmi_ok = true;
            int mode = -1, bm = -1;
            if (parse_cnmi(resp, &mode, &bm) && (mode != 2 || bm != 1)) {
                /* URC configuration was lost without the cached flag noticing.
                 * Reapply it; the next probe verifies the result. */
                ESP_LOGW(TAG, "%s CNMI=%d,%d, reapplying", modem_name(m), mode, bm);
                esp_err_t cfg_err = sms_modem_configure(slot);
                if (cfg_err == ESP_OK) {
                    cnmi_ok = true;
                    taskENTER_CRITICAL(&s_status_mux);
                    s_status[slot].repairs++;
                    taskEXIT_CRITICAL(&s_status_mux);
                } else {
                    cnmi_ok = false;
                    if (cfg_err == ESP_ERR_TIMEOUT || cfg_err == MODEM_ERR_DESYNC)
                        sms_dead = true;
                    ESP_LOGW(TAG, "%s CNMI reapply failed (%d)", modem_name(m), (int)cfg_err);
                }
            }
        } else {
            if (e == ESP_ERR_TIMEOUT || e == MODEM_ERR_DESYNC) sms_dead = true;
            ESP_LOGW(TAG, "%s CNMI probe failed (%d)", modem_name(m), (int)e);
        }
    }

    int64_t now = now_ms();
    bool reset_at = false, reset_sms = false;
    taskENTER_CRITICAL(&s_status_mux);
    modem_health_status_t *st = &s_status[slot];
    st->probes++;
    st->last_probe_ms = now;
    st->at_ok = at_ok;
    st->cpms_ok = cpms_ok;
    st->cnmi_ok = cnmi_ok;
    if (!at_alive) {
        st->at_fail_streak++;
        if (st->at_fail_streak >= PROBE_THRESHOLD) {
            st->at_fail_streak = 0;
            reset_at = true;
        }
    } else {
        st->at_fail_streak = 0;
        if (sms_dead) {
            st->sms_fail_streak++;
            if (st->sms_fail_streak >= PROBE_THRESHOLD) {
                st->sms_fail_streak = 0;
                reset_sms = true;
            }
        } else {
            st->sms_fail_streak = 0;
        }
    }
    int at_fail = st->at_fail_streak, sms_fail = st->sms_fail_streak;
    taskEXIT_CRITICAL(&s_status_mux);

    if (reset_at) {
        ESP_LOGE(TAG, "%s AT dead, hardware reset", modem_name(m));
        if (!reset_modem(slot, MODEM_RESET_REASON_AT)) s_next_probe = now + PROBE_RETRY_MS;
    } else if (reset_sms) {
        ESP_LOGE(TAG, "%s SMS path stuck (AT alive), hardware reset", modem_name(m));
        if (!reset_modem(slot, MODEM_RESET_REASON_SMS)) s_next_probe = now + PROBE_RETRY_MS;
    } else if (!at_alive || sms_dead) {
        ESP_LOGW(TAG, "%s probe failure (at=%d sms=%d/%d)", modem_name(m),
                 at_fail, sms_fail, PROBE_THRESHOLD);
    }
}

static void process_manual_resets(int64_t now)
{
    uint8_t mask;
    taskENTER_CRITICAL(&s_status_mux);
    mask = s_manual_reset_mask;
    s_manual_reset_mask = 0;
    taskEXIT_CRITICAL(&s_status_mux);
    for (int i = 0; i < MODEM_COUNT; ++i) {
        if (!(mask & (1u << i))) continue;
        if (sms_call_active()) {
            /* The call may have started after the RPC checked; never drop it. */
            ESP_LOGW(TAG, "%s manual reset skipped: call active", modem_name(&s_modems[i]));
            continue;
        }
        if (!reset_modem(i, MODEM_RESET_REASON_MANUAL))
            ESP_LOGW(TAG, "%s manual reset skipped", modem_name(&s_modems[i]));
    }
    (void)now;
}

static void process_maintenance(int64_t now)
{
    uint8_t hours = cfg_get()->modem_reset_hours;
    if (hours != s_maint_hours) {
        s_maint_hours = hours;
        for (int i = 0; i < MODEM_COUNT; ++i)
            s_next_maint[i] = hours ? now + (int64_t)hours * 3600000 + i * MAINT_STAGGER_MS : 0;
        ESP_LOGI(TAG, "maintenance reset: %u h", (unsigned)hours);
    }
    if (!hours) return;
    for (int i = 0; i < MODEM_COUNT; ++i) {
        if (now < s_next_maint[i]) continue;
        if (firmware_busy() || sms_call_active()) {
            /* Never drop a call or a firmware upload for routine maintenance. */
            s_next_maint[i] = now + MAINT_RETRY_MS;
            ESP_LOGI(TAG, "%s maintenance reset deferred (busy)", modem_name(&s_modems[i]));
            continue;
        }
        ESP_LOGW(TAG, "%s maintenance reset", modem_name(&s_modems[i]));
        if (reset_modem(i, MODEM_RESET_REASON_SCHEDULED))
            s_next_maint[i] = now + (int64_t)hours * 3600000;
        else
            s_next_maint[i] = now + MAINT_RETRY_MS;
    }
}

static void health_task(void *arg)
{
    (void)arg;
    s_health_task = xTaskGetCurrentTaskHandle();

    while (1) {
        int64_t now = now_ms();

        if (s_probe_now) {
            s_probe_now = false;
            s_next_probe = now;
        }
        process_manual_resets(now);
        process_maintenance(now);

        if (now >= s_next_probe) {
            s_next_probe = now + PROBE_INTERVAL_MS;
            for (int i = 0; i < MODEM_COUNT; i++) {
                probe_slot(i);
                if (!sms_modem_is_configured(i)) {
                    esp_err_t cfg_err = sms_modem_configure(i);
                    if (cfg_err != ESP_OK)
                        ESP_LOGW(TAG, "%s SMS setup retry failed (%d)",
                                 modem_name(&s_modems[i]), (int)cfg_err);
                }
            }
        }

        /* LED */
        static int tick;
        tick++;
        /* Solid only when the long connection is actually delivering: a session that
         * is connected but silent is exactly what the field outage looked like. */
        bool fs_ok = feishu_ws_connected() && feishu_ws_alive();

        gpio_set_level(PIN_LED1, fs_ok ? 1 : (tick % 4 < 2));
        if (s_blink_sms > 0) {
            gpio_set_level(PIN_LED2, 1);
            s_blink_sms--;
        } else if (s_blink_err > 0) {
            gpio_set_level(PIN_LED2, tick % 2);
            s_blink_err--;
        } else {
            gpio_set_level(PIN_LED2, 0);
        }

        /* Wake immediately for a manual probe/reset, otherwise 1 Hz heartbeat. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

void health_start(modem_t *modems)
{
    s_modems = modems;
    memset(s_status, 0, sizeof(s_status));
    s_manual_reset_mask = 0;
    s_probe_now = false;
    s_blink_sms = 0;
    s_blink_err = 0;
    s_maint_hours = cfg_get()->modem_reset_hours;
    int64_t now = now_ms();
    for (int i = 0; i < MODEM_COUNT; ++i)
        s_next_maint[i] = s_maint_hours
            ? now + (int64_t)s_maint_hours * 3600000 + i * MAINT_STAGGER_MS : 0;
    s_next_probe = now + 30000;
    if (xTaskCreate(health_task, "health", 4096, NULL, 3, &s_health_task) != pdPASS) {
        ESP_LOGE(TAG, "health task create failed");
        s_health_task = NULL;
    }
}
