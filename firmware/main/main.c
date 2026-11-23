#include "modem_timing.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "board.h"
#include "modem.h"
#include "console.h"
#include "sms.h"
#include "cmd.h"
#include "feishu.h"
#include "cmd_policy.h"
#include "cfg.h"
#include "wifi.h"
#include "ble.h"
#include "lan_web.h"
#include "rpc.h"
#include "health.h"
#include "nvs_flash.h"
#include "device_status.h"
#include "storage.h"
#include "runtime.h"
#include "power_policy.h"
#include "factory_reset.h"
#include "firmware_update.h"
#include "ble_lifecycle_policy.h"

static const char *TAG = "app";

/* 鍚姩澶辫触璁℃暟锛氳蒋浠跺浣嶄笉娓呴浂锛岀敤浜庤嚜鍔ㄨ繘鍏ュ畨鍏ㄦā寮?*/
#define BOOT_MAGIC 0x5A5A5A5A
RTC_NOINIT_ATTR static uint32_t s_boot_magic;
RTC_NOINIT_ATTR static uint32_t s_boot_fail;

static modem_t s_modems[MODEM_COUNT];
static bool s_safe_mode;
static TaskHandle_t s_net_task;
device_slot_status_t device_slots[2];
static char s_slot_sim[MODEM_COUNT][32];
static char s_slot_iccid[MODEM_COUNT][32];

static void refresh_iccid_label(int slot)
{
    char resp[128] = {0}, digits[32];
    if (modem_cmd(&s_modems[slot], "AT+ICCID", resp, sizeof(resp), AT_SIM_IDENTITY_MS) != ESP_OK) return;
    size_t n = 0;
    for (const char *p = resp; *p && n + 1 < sizeof(digits); ++p)
        if (*p >= '0' && *p <= '9') digits[n++] = *p;
    digits[n] = '\0';
    if (n >= 6) snprintf(s_slot_iccid[slot], sizeof(s_slot_iccid[slot]), "卡%d-%s", slot + 1, digits + n - 6);
}

static bool quoted_field(const char *src, int field, char *out, size_t cap)
{
    const char *p = src;
    for (int i = 0; i <= field; i++) {
        p = strchr(p, '"'); if (!p) return false;
        const char *e = strchr(++p, '"'); if (!e) return false;
        if (i == field) { size_t n = e - p; if (!n || n >= cap) return false; memcpy(out, p, n); out[n] = 0; return true; }
        p = e + 1;
    }
    return false;
}

esp_err_t cmd_refresh_slot_number(int i, char *number, size_t number_len)
{
    if (i < 0 || i >= MODEM_COUNT || !number || number_len == 0) return ESP_ERR_INVALID_ARG;
    number[0] = '\0';
    s_slot_sim[i][0] = '\0';
    s_slot_iccid[i][0] = '\0';
    cmd_set_slot(i, NULL, "未知");

    char resp[512] = {0}, candidate[32] = {0}, key[12];
    esp_err_t e = modem_cmd(&s_modems[i], "AT+CNUM", resp, sizeof(resp), AT_SIM_IDENTITY_MS);
    if (e == ESP_OK && quoted_field(resp, 1, candidate, sizeof(candidate)) &&
        cmd_policy_local_number(candidate, key, sizeof(key))) {
        strlcpy(s_slot_sim[i], candidate, sizeof(s_slot_sim[i]));
    } else {
        /* Never fall back to a cached identity after a busy, timeout, or desync. */
        if (e != ESP_OK && e != ESP_FAIL) return e;
        memset(resp, 0, sizeof(resp));
        e = modem_cmd(&s_modems[i], "AT+CPBS=\"ON\"", resp, sizeof(resp), AT_SETUP_MS);
        if (e != ESP_OK && e != ESP_FAIL) return e;
        if (e == ESP_OK) {
            memset(resp, 0, sizeof(resp));
            e = modem_cmd(&s_modems[i], "AT+CPBR=1,20", resp, sizeof(resp), 5000);
            if (e == ESP_OK && quoted_field(resp, 0, candidate, sizeof(candidate)) &&
                cmd_policy_local_number(candidate, key, sizeof(key))) {
                strlcpy(s_slot_sim[i], candidate, sizeof(s_slot_sim[i]));
            } else if (e != ESP_OK && e != ESP_FAIL) {
                return e;
            }
        }
    }

    const app_cfg_t *c = cfg_get();
    bool manual = cmd_policy_local_number(c->slot_num[i], key, sizeof(key));
    if (!s_slot_sim[i][0] && !manual) refresh_iccid_label(i);
    const char *resolved = s_slot_sim[i][0] ? s_slot_sim[i] :
        manual ? c->slot_num[i] : s_slot_iccid[i][0] ? s_slot_iccid[i] : "未知";
    bool has_number = s_slot_sim[i][0] || manual;
    if (strlcpy(number, resolved, number_len) >= number_len) return ESP_ERR_INVALID_SIZE;
    cmd_set_slot(i, NULL, resolved);
    return has_number ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void resolve_slot_number(int i)
{
    char number[32];
    esp_err_t e = cmd_refresh_slot_number(i, number, sizeof(number));
    if (e != ESP_OK) ESP_LOGW(TAG, "SIM %d phone number unavailable (%d)", i + 1, (int)e);
}

void device_slot_config_changed(int slot, const char *name, const char *number)
{
    if (slot < 0 || slot >= MODEM_COUNT) return;
    char key[12];
    const char *resolved = s_slot_sim[slot][0] ? s_slot_sim[slot] :
                           (number && cmd_policy_local_number(number, key, sizeof(key))) ? number :
                           s_slot_iccid[slot][0] ? s_slot_iccid[slot] : "未知";
    cmd_set_slot(slot, name, resolved);
}

void network_config_changed(void)
{
    if (s_net_task) xTaskNotifyGive(s_net_task);
}

bool device_local_recovery_mode(void)
{
    return s_safe_mode;
}

static int response_int(const char *s, const char *key)
{
    const char *p = strstr(s, key); if (!p) return -1;
    p += strlen(key); while (*p && (*p < '0' || *p > '9')) p++;
    return *p ? atoi(p) : -1;
}

static void slot_status_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(20000));
    while (1) {
        for (int i = 0; i < MODEM_COUNT; i++) {
            char r[256] = {0};
            if (modem_cmd(&s_modems[i], "AT+CEREG?", r, sizeof(r), AT_REGISTRATION_MS) == ESP_OK) {
                int stat = cmd_policy_cereg_stat(r);
                device_slots[i].registered = stat == 1 || stat == 5;
                device_slots[i].operator_name[0] = 0;
                device_slots[i].csq = -1;
                memset(r, 0, sizeof(r));
                if (modem_cmd(&s_modems[i], "AT+COPS?", r, sizeof(r), AT_OPERATOR_MS) == ESP_OK)
                    quoted_field(r, 0, device_slots[i].operator_name, sizeof(device_slots[i].operator_name));
                memset(r, 0, sizeof(r));
                if (modem_cmd(&s_modems[i], "AT+CSQ", r, sizeof(r), AT_SIGNAL_MS) == ESP_OK) {
                    int q = response_int(r, "+CSQ:");
                    device_slots[i].csq = q >= 0 && q <= 31 ? q : -1;
                }
                device_slots[i].valid = true;
            } else device_slots[i].valid = false;
        }
        /* Keep dashboard status useful without polling both modems too often. */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void boot_ok_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(60000));
    s_boot_fail = 0;
    ESP_LOGI(TAG, "boot healthy, failure counter cleared");
    vTaskDelete(NULL);
}

static void leds_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LED1) | (1ULL << PIN_LED2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(PIN_LED1, 0);
    gpio_set_level(PIN_LED2, 0);
}

static void modems_boot_task(void *arg)
{
    (void)arg;
    if (s_safe_mode) {
        vTaskDelete(NULL);
        return;
    }
    /* Reset the modules once after power-up; otherwise SIM detection may stall. */
    for (int i = 0; i < MODEM_COUNT; i++) {
        modem_reset_hw(&s_modems[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(12000));

    for (int i = 0; i < MODEM_COUNT; i++) {
        char resp[256] = {0};
        esp_err_t link = ESP_FAIL;
        for (int attempt = 0; attempt < 5; ++attempt) {
            link = modem_cmd(&s_modems[i], "AT", NULL, 0, AT_SETUP_MS);
            if (link == ESP_OK) break;
            if (attempt < 4) vTaskDelay(50);  /* Factory handshake delay in RTOS ticks. */
        }
        if (link != ESP_OK) {
            ESP_LOGW(TAG, "%s startup handshake unavailable; background recovery will retry",
                     modem_name(&s_modems[i]));
            continue;
        }
        modem_cmd(&s_modems[i], "AT+CPIN?", resp, sizeof(resp), AT_SIM_IDENTITY_MS);
        ESP_LOGI(TAG, "%s CPIN: %s", modem_name(&s_modems[i]), resp);
        memset(resp, 0, sizeof(resp));
        modem_cmd(&s_modems[i], "AT+CEREG?", resp, sizeof(resp), AT_REGISTRATION_MS);
        ESP_LOGI(TAG, "%s CEREG: %s", modem_name(&s_modems[i]), resp);

        resolve_slot_number(i);
    }

    esp_err_t sms_err = sms_init(s_modems);
    if (sms_err != ESP_OK) {
        ESP_LOGE(TAG, "SMS service init failed: %s; boot not marked healthy", esp_err_to_name(sms_err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "modems ready");
    if (!s_safe_mode) xTaskCreate(boot_ok_task, "boot_ok", 2048, NULL, 3, NULL);
    /* The receive loop inherits this stack. Wi-Fi, TLS and BLE can all be
     * starting here; allocating a second 24 KiB stack can starve SMS/calls. */
    sms_run();
}

static void net_task(void *arg)
{
    (void)arg;
    int64_t fs_retry_at = 0;
    bool ntp_started = false;
    bool was_online = false;
    bool had_ip = false;
    bool lan_started = false;
    int64_t wifi_start_retry_at = 0;
    int64_t lan_start_retry_at = 0;
    uint32_t wifi_start_retry_ms = 1000;
    uint32_t lan_start_retry_ms = 1000;
    while (1) {
        const app_cfg_t *cfg = cfg_get();
        int64_t now = esp_timer_get_time() / 1000;
        bool has_ip = wifi_connected();
        if (had_ip && !has_ip) {
            lan_web_stop();
            feishu_ws_stop();
            lan_started = false;
            lan_start_retry_at = 0;
            lan_start_retry_ms = 1000;
        }
        if (had_ip != has_ip) fs_retry_at = 0;
        had_ip = has_ip;
        if (cfg->wifi_ssid[0] && !wifi_config_applied() && now >= wifi_start_retry_at) {
            esp_err_t e = wifi_start(cfg);
            if (e == ESP_OK) {
                wifi_start_retry_ms = 1000;
            } else {
                wifi_start_retry_at = now + wifi_start_retry_ms;
                wifi_start_retry_ms = wifi_start_retry_ms < 15000
                    ? wifi_start_retry_ms * 2 : 30000;
            }
        }
        if (has_ip && !lan_started && now >= lan_start_retry_at) {
            esp_err_t web_err = lan_web_start();
            if (web_err == ESP_OK) {
                lan_started = true;
                lan_start_retry_ms = 1000;
            } else {
                ESP_LOGW(TAG, "LAN management server start deferred (%s)", esp_err_to_name(web_err));
                lan_start_retry_at = now + lan_start_retry_ms;
                lan_start_retry_ms = lan_start_retry_ms < 15000 ? lan_start_retry_ms * 2 : 30000;
            }
        }
        if (has_ip && !ntp_started) { sntp_start(); ntp_started = true; }
        bool online = has_ip && time_synced() && feishu_ws_connected();
        if (has_ip && time_synced() && feishu_configured() && !feishu_ws_connected() && now >= fs_retry_at) {
            esp_err_t e = feishu_ws_start();
            fs_retry_at = now + (e == ESP_OK ? 5000 : 8000);
        }
        bool setup_done = cfg_setup_done();
        if (ble_lifecycle_can_complete(setup_done, has_ip, feishu_ws_connected()) &&
            rpc_complete_setup()) {
            ESP_LOGI(TAG, "Wi-Fi and authenticated Feishu websocket are online; closing BLE setup");
            ble_lockdown();
        }
        if (online && !was_online) sms_retry_pending();
        was_online = online;
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))) {
            fs_retry_at = 0;
            wifi_start_retry_at = 0;
            wifi_start_retry_ms = 1000;
            lan_start_retry_at = 0;
            lan_start_retry_ms = 1000;
        }
    }
}

static void on_peer_learned(const char *open_id, const char *chat_id)
{
    app_cfg_t *c = cfg_mut();
    if (open_id) strlcpy(c->open_id, open_id, sizeof(c->open_id));
    if (chat_id) strlcpy(c->chat_id, chat_id, sizeof(c->chat_id));
    cfg_save();
    ESP_LOGI(TAG, "peer persisted");
}

void app_main(void)
{
    firmware_boot_check_start(lan_web_ready);
    esp_err_t pm_err = power_policy_init();
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "power policy unavailable (%s)", esp_err_to_name(pm_err));
    }
    char power_status[48];
    power_policy_status(power_status, sizeof(power_status));
    ESP_LOGI(TAG, "clock %s", power_status);

    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (s_boot_magic != BOOT_MAGIC) {
        s_boot_magic = BOOT_MAGIC;
        s_boot_fail = 0;
    }
    bool intentional_reset = reset_reason == ESP_RST_SW || reset_reason == ESP_RST_EXT ||
                             reset_reason == ESP_RST_DEEPSLEEP;
    if (intentional_reset) s_boot_fail = 0;
    else s_boot_fail++;

    leds_init();

    esp_err_t nerr;
    while ((nerr = nvs_flash_init()) != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s; startup is held for safe recovery",
                 esp_err_to_name(nerr));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    esp_err_t reset_err;
    while ((reset_err = factory_reset_recover()) != ESP_OK) {
        ESP_LOGE(TAG, "factory-reset recovery failed: %s; startup is held",
                 esp_err_to_name(reset_err));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    esp_err_t cfg_err = cfg_init();
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "configuration load failed: %s; startup is held", esp_err_to_name(cfg_err));
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }
    esp_err_t storage_err = storage_mount();
    if (storage_err != ESP_OK) ESP_LOGE(TAG, "storage unavailable: %s", esp_err_to_name(storage_err));

    const app_cfg_t *cfg = cfg_get();
    feishu_cfg_t fcfg = {0};
    strlcpy(fcfg.app_id, cfg->app_id, sizeof(fcfg.app_id));
    strlcpy(fcfg.app_secret, cfg->app_secret, sizeof(fcfg.app_secret));
    strlcpy(fcfg.open_id, cfg->open_id, sizeof(fcfg.open_id));
    strlcpy(fcfg.chat_id, cfg->chat_id, sizeof(fcfg.chat_id));
    feishu_init(&fcfg);

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    printf("\n\n=== newSMSForward bring-up ===\n");
    printf("chip %d core(s) rev %d, flash %lu MB, mac %02X:%02X:%02X:%02X:%02X:%02X\n",
           ci.cores, ci.revision, (unsigned long)(flash / (1024 * 1024)),
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("boot #%lu, reset reason %d\n", (unsigned long)s_boot_fail,
           (int)esp_reset_reason());

    if (s_boot_fail >= 3) {
        s_safe_mode = true;
        printf("SAFE MODE (boot failure x%lu): console and local recovery only\n",
               (unsigned long)s_boot_fail);
    }

    gpio_set_level(PIN_LED1, 1);

    for (int i = 0; i < MODEM_COUNT; i++) {
        esp_err_t err = modem_init(&s_modems[i], i);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "modem%d init failed: %s", i + 1, esp_err_to_name(err));
        }
    }

    console_start(s_modems);

    gpio_set_level(PIN_LED1, 0);
    gpio_set_level(PIN_LED2, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_LED2, 0);

    ESP_LOGI(TAG, "ready, type 'help' then 'pa'");
    cmd_init(s_modems);
    esp_err_t rpc_err = rpc_init();
    if (rpc_err != ESP_OK) ESP_LOGE(TAG, "RPC init failed: %s", esp_err_to_name(rpc_err));
    cmd_set_slot(0, cfg->slot_name[0], cfg->slot_num[0]);
    cmd_set_slot(1, cfg->slot_name[1], cfg->slot_num[1]);
    if (!s_safe_mode) {
        feishu_set_cmd_cb(cmd_handle);
        feishu_set_peer_cb(on_peer_learned);
        wifi_preinit(); /* Initialize the driver only; net_task owns the first start. */
        xTaskCreate(modems_boot_task, "modem_sms", 24576, NULL, 6, NULL);
        xTaskCreate(slot_status_task, "slot_status", 4096, NULL, 3, NULL);
        xTaskCreate(net_task, "net", 6144, NULL, 4, &s_net_task);
        health_start(s_modems);
    }
    ble_start();
    if (storage_err == ESP_OK && rpc_err == ESP_OK && !s_safe_mode)
        firmware_boot_check_ready();
}
