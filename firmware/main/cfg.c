#include "cfg.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";
static const char *NS = "smsfwd";
static app_cfg_t s_cfg;

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
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

esp_err_t cfg_init(void)
{
    cfg_defaults();

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

    nvs_close(h);
    cfg_apply_timezone();
    ESP_LOGI(TAG, "config loaded (wifi=%s, feishu=%s)",
             s_cfg.wifi_ssid[0] ? "configured" : "unset",
             s_cfg.app_id[0] ? "configured" : "unset");
    return ESP_OK;
}

esp_err_t cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

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
    if (err == ESP_OK) ESP_LOGI(TAG, "config saved");
    #undef SET
    return err;
}

const app_cfg_t *cfg_get(void) { return &s_cfg; }
app_cfg_t *cfg_mut(void) { return &s_cfg; }
