#include "cfg.h"
#include "factory_reset.h"

#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cfg";
static const char *NS = "smsfwd";
static const char SETUP_KEY[] = "setup_done";
static app_cfg_t s_cfg;
static atomic_bool s_setup_done;
static SemaphoreHandle_t s_save_lock;

void cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.dhcp = true;
    s_cfg.tz_offset = 8;
    strlcpy(s_cfg.slot_name[0], "卡1", sizeof(s_cfg.slot_name[0]));
    strlcpy(s_cfg.slot_name[1], "卡2", sizeof(s_cfg.slot_name[1]));
}

void cfg_apply_timezone(void)
{
    char tz[16];
    int offset = s_cfg.tz_offset;
    snprintf(tz, sizeof(tz), "UTC%+d", -offset);
    setenv("TZ", tz, 1);
    tzset();
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    char value[sizeof(s_cfg.app_secret)];
    size_t n = len;
    if (len <= sizeof(value) && nvs_get_str(h, key, value, &n) == ESP_OK)
        memcpy(out, value, n);
}

esp_err_t cfg_init(void)
{
    cfg_defaults();
    atomic_store(&s_setup_done, false);
    if (!s_save_lock) s_save_lock = xSemaphoreCreateMutex();
    if (!s_save_lock) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no config yet, defaults");
        cfg_apply_timezone();
        return ESP_OK;
    }

    get_str(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    get_str(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    get_str(h, "ip", s_cfg.ip, sizeof(s_cfg.ip));
    get_str(h, "mask", s_cfg.mask, sizeof(s_cfg.mask));
    get_str(h, "gw", s_cfg.gw, sizeof(s_cfg.gw));
    get_str(h, "dns", s_cfg.dns, sizeof(s_cfg.dns));
    get_str(h, "app_id", s_cfg.app_id, sizeof(s_cfg.app_id));
    get_str(h, "app_secret", s_cfg.app_secret, sizeof(s_cfg.app_secret));
    get_str(h, "open_id", s_cfg.open_id, sizeof(s_cfg.open_id));
    get_str(h, "chat_id", s_cfg.chat_id, sizeof(s_cfg.chat_id));
    get_str(h, "slot1_name", s_cfg.slot_name[0], sizeof(s_cfg.slot_name[0]));
    get_str(h, "slot2_name", s_cfg.slot_name[1], sizeof(s_cfg.slot_name[1]));
    get_str(h, "slot1_num", s_cfg.slot_num[0], sizeof(s_cfg.slot_num[0]));
    get_str(h, "slot2_num", s_cfg.slot_num[1], sizeof(s_cfg.slot_num[1]));

    uint8_t v = 1;
    nvs_get_u8(h, "dhcp", &v);
    s_cfg.dhcp = v != 0;
    int8_t tz = 8;
    nvs_get_i8(h, "tz", &tz);
    s_cfg.tz_offset = tz;
    uint8_t setup = 0;
    if (nvs_get_u8(h, SETUP_KEY, &setup) == ESP_OK) atomic_store(&s_setup_done, setup == 1);

    nvs_close(h);
    cfg_apply_timezone();
    ESP_LOGI(TAG, "config loaded (wifi=%s, feishu=%s)",
             s_cfg.wifi_ssid[0] ? "configured" : "unset",
             s_cfg.app_id[0] ? "configured" : "unset");
    return ESP_OK;
}

esp_err_t cfg_save(void)
{
    if (!s_save_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = factory_reset_pending() ? ESP_ERR_INVALID_STATE : nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_save_lock);
        return err;
    }

    #define SET(call) do { err = (call); if (err != ESP_OK) goto done; } while (0)
    SET(nvs_set_str(h, "wifi_ssid", s_cfg.wifi_ssid));
    SET(nvs_set_str(h, "wifi_pass", s_cfg.wifi_pass));
    SET(nvs_set_str(h, "ip", s_cfg.ip));
    SET(nvs_set_str(h, "mask", s_cfg.mask));
    SET(nvs_set_str(h, "gw", s_cfg.gw));
    SET(nvs_set_str(h, "dns", s_cfg.dns));
    SET(nvs_set_u8(h, "dhcp", s_cfg.dhcp ? 1 : 0));
    SET(nvs_set_str(h, "app_id", s_cfg.app_id));
    SET(nvs_set_str(h, "app_secret", s_cfg.app_secret));
    SET(nvs_set_str(h, "open_id", s_cfg.open_id));
    SET(nvs_set_str(h, "chat_id", s_cfg.chat_id));
    SET(nvs_set_str(h, "slot1_name", s_cfg.slot_name[0]));
    SET(nvs_set_str(h, "slot2_name", s_cfg.slot_name[1]));
    SET(nvs_set_str(h, "slot1_num", s_cfg.slot_num[0]));
    SET(nvs_set_str(h, "slot2_num", s_cfg.slot_num[1]));
    SET(nvs_set_i8(h, "tz", s_cfg.tz_offset));

    err = nvs_commit(h);
done:
    nvs_close(h);
    xSemaphoreGive(s_save_lock);
    if (err == ESP_OK) ESP_LOGI(TAG, "config saved");
    #undef SET
    return err;
}

bool cfg_setup_done(void)
{
    return atomic_load(&s_setup_done);
}

esp_err_t cfg_read_setup_credentials(app_cfg_t *out)
{
    if (!s_save_lock || !out) return ESP_ERR_INVALID_STATE;
    *out = (app_cfg_t){ .dhcp = true };
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        get_str(nvs, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
        get_str(nvs, "wifi_pass", out->wifi_pass, sizeof(out->wifi_pass));
        get_str(nvs, "ip", out->ip, sizeof(out->ip));
        get_str(nvs, "app_id", out->app_id, sizeof(out->app_id));
        get_str(nvs, "app_secret", out->app_secret, sizeof(out->app_secret));
        uint8_t dhcp = 1;
        if (nvs_get_u8(nvs, "dhcp", &dhcp) == ESP_OK) out->dhcp = dhcp != 0;
        nvs_close(nvs);
    }
    xSemaphoreGive(s_save_lock);
    return err;
}

static bool matches_saved(nvs_handle_t nvs, const char *key, const char *value)
{
    char saved[sizeof(s_cfg.app_secret)];
    size_t size = sizeof(saved);
    esp_err_t err = nvs_get_str(nvs, key, saved, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return !*value;
    return err == ESP_OK && strcmp(saved, value) == 0;
}

esp_err_t cfg_commit_setup(const app_cfg_t *expected)
{
    if (!s_save_lock || !expected || !expected->wifi_ssid[0] || !expected->app_id[0] ||
        !expected->app_secret[0]) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!factory_reset_pending()) {
        if (cfg_setup_done()) err = ESP_OK;
        else {
            nvs_handle_t nvs;
            err = nvs_open(NS, NVS_READWRITE, &nvs);
            if (err == ESP_OK) {
                uint8_t dhcp = 1;
                (void)nvs_get_u8(nvs, "dhcp", &dhcp);
                if (expected->dhcp != (dhcp != 0) ||
                    (!expected->dhcp && !matches_saved(nvs, "ip", expected->ip)) ||
                    !matches_saved(nvs, "wifi_ssid", expected->wifi_ssid) ||
                    !matches_saved(nvs, "wifi_pass", expected->wifi_pass) ||
                    !matches_saved(nvs, "app_id", expected->app_id) ||
                    !matches_saved(nvs, "app_secret", expected->app_secret)) {
                    err = ESP_ERR_INVALID_STATE;
                } else {
                    err = nvs_set_u8(nvs, SETUP_KEY, 1);
                    if (err == ESP_OK) err = nvs_commit(nvs);
                    if (err == ESP_OK) atomic_store(&s_setup_done, true);
                }
                nvs_close(nvs);
            }
        }
    }
    xSemaphoreGive(s_save_lock);
    return err;
}

esp_err_t cfg_erase_persisted(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) atomic_store(&s_setup_done, false);
    return err;
}

const app_cfg_t *cfg_get(void) { return &s_cfg; }
app_cfg_t *cfg_mut(void) { return &s_cfg; }
