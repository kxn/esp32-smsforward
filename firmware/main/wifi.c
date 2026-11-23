#include "wifi.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"

static const char *TAG = "wifi";

#define BIT_CONNECTED BIT0
#define BIT_GOT_IP BIT1

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_config_lock;
static bool s_inited;
static bool s_started;
static bool s_configured;
static volatile bool s_time_synced;
static char s_ip[16] = "";
static int s_rssi;
static int64_t s_up_since;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_delay_ms = 1000;

#define WIFI_RETRY_INITIAL_MS 1000
#define WIFI_RETRY_MAX_MS 30000

static void retry_connect(void *arg) { (void)arg; esp_wifi_connect(); }

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (!s_retry_timer) {
                const esp_timer_create_args_t args = { .callback = retry_connect, .name = "wifi_retry" };
                esp_timer_create(&args, &s_retry_timer);
            }
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
            s_ip[0] = '\0';
            uint32_t delay_ms = s_retry_delay_ms;
            s_retry_delay_ms = s_retry_delay_ms < WIFI_RETRY_MAX_MS / 2
                ? s_retry_delay_ms * 2 : WIFI_RETRY_MAX_MS;
            ESP_LOGW(TAG, "disconnected, retry in %lu ms", (unsigned long)delay_ms);
            if (s_retry_timer) {
                esp_timer_stop(s_retry_timer);
                esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
            }
        } else if (id == WIFI_EVENT_STA_STOP) {
            xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
            s_ip[0] = '\0';
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
        s_up_since = esp_timer_get_time() / 1000000;
        s_retry_delay_ms = WIFI_RETRY_INITIAL_MS;
        if (s_retry_timer) esp_timer_stop(s_retry_timer);
        ESP_LOGI(TAG, "got ip %s", s_ip);
    }
}

static void netif_init_once(void)
{
    if (s_inited) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    s_events = xEventGroupCreate();
    s_config_lock = xSemaphoreCreateMutex();
    s_inited = true;
}

esp_err_t wifi_preinit(void)
{
    netif_init_once();
    return ESP_OK;
}

static esp_err_t wifi_apply_config(const app_cfg_t *cfg, bool force_restart)
{
    if (!cfg->wifi_ssid[0]) {
        ESP_LOGW(TAG, "no ssid configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(cfg->wifi_ssid) > 32 || strlen(cfg->wifi_pass) > 63) return ESP_ERR_INVALID_ARG;
    netif_init_once();
    if (!s_config_lock) return ESP_ERR_NO_MEM;

    esp_netif_ip_info_t static_ip = {0};
    esp_netif_dns_info_t static_dns = {0};
    if (!cfg->dhcp) {
        if (!cfg->ip[0] || !cfg->mask[0] || !cfg->gw[0] ||
            esp_netif_str_to_ip4(cfg->ip, &static_ip.ip) != ESP_OK ||
            esp_netif_str_to_ip4(cfg->mask, &static_ip.netmask) != ESP_OK ||
            esp_netif_str_to_ip4(cfg->gw, &static_ip.gw) != ESP_OK ||
            !static_ip.ip.addr || !static_ip.netmask.addr || !static_ip.gw.addr)
            return ESP_ERR_INVALID_ARG;
        static_dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(cfg->dns[0] ? cfg->dns : cfg->gw,
                                 &static_dns.ip.u_addr.ip4) != ESP_OK ||
            !static_dns.ip.u_addr.ip4.addr) return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    if (s_started && s_configured && !force_restart) {
        xSemaphoreGive(s_config_lock);
        return ESP_OK;
    }

    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, cfg->wifi_ssid, strlen(cfg->wifi_ssid));
    strlcpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    if (s_started) {
        if (s_retry_timer) esp_timer_stop(s_retry_timer);
        esp_wifi_stop();
        s_started = false;
        s_configured = false;
    }
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) goto done;
    e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) goto done;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) { e = ESP_ERR_INVALID_STATE; goto done; }
    e = esp_netif_dhcpc_stop(netif);
    if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) goto done;

    if (!cfg->dhcp && cfg->ip[0]) {
        e = esp_netif_set_ip_info(netif, &static_ip);
        if (e != ESP_OK) goto done;
        e = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &static_dns);
        if (e != ESP_OK) goto done;
    } else {
        esp_netif_ip_info_t empty = {0};
        e = esp_netif_set_ip_info(netif, &empty);
        if (e != ESP_OK) goto done;
        e = esp_netif_dhcpc_start(netif);
        if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) goto done;
    }

    s_retry_delay_ms = WIFI_RETRY_INITIAL_MS;
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) { e = start_err; goto done; }
    s_started = true;
    s_configured = true;
    ESP_LOGI(TAG, "connecting to %s", cfg->wifi_ssid);
    e = ESP_OK;
done:
    xSemaphoreGive(s_config_lock);
    return e;
}

esp_err_t wifi_start(const app_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    return wifi_apply_config(cfg, false);
}

esp_err_t wifi_reconfigure(const app_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    return wifi_apply_config(cfg, true);
}

bool wifi_config_applied(void)
{
    if (!s_config_lock) return false;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    bool started = s_configured;
    xSemaphoreGive(s_config_lock);
    return started;
}

bool wifi_connected(void)
{
    if (!s_events) return false;
    return (xEventGroupGetBits(s_events) & BIT_GOT_IP) != 0;
}

uint32_t wifi_uptime_seconds(void)
{
    if (!wifi_connected() || !s_up_since) return 0;
    return (uint32_t)(esp_timer_get_time() / 1000000 - s_up_since);
}

void wifi_status(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(ssid, (const char *)ap.ssid, ssid_len);
        s_rssi = ap.rssi;
    } else {
        if (ssid && ssid_len) ssid[0] = '\0';
        s_rssi = 0;
    }
    strlcpy(ip, s_ip, ip_len);
    if (rssi) *rssi = s_rssi;
}

static void time_cb(struct timeval *tv)
{
    s_time_synced = true;
    ESP_LOGI(TAG, "sntp synced: %lld", (long long)tv->tv_sec);
}

esp_err_t sntp_start(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "ntp.tencent.com");
    esp_sntp_setservername(2, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_cb);
    esp_sntp_init();
    return ESP_OK;
}

bool time_synced(void)
{
    return s_time_synced;
}

esp_err_t wifi_scan(char *json, size_t json_len)
{
    if (!json || !json_len) return ESP_ERR_INVALID_ARG;
    netif_init_once();
    if (!s_config_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (!s_started) {
        wifi_config_t empty_cfg = {0};
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
        if (err == ESP_OK) {
            err = esp_wifi_start();
            if (err == ESP_OK) {
                s_started = true;
                s_configured = false;
            }
        }
        if (err != ESP_OK) goto done;
    }

    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) goto done;

    uint16_t count = 16;
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (!records) { err = ESP_ERR_NO_MEM; goto done; }
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) { free(records); goto done; }

    cJSON *array = cJSON_CreateArray();
    if (!array) { free(records); err = ESP_ERR_NO_MEM; goto done; }
    for (uint16_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid) ||
            !cJSON_AddNumberToObject(item, "rssi", records[i].rssi)) {
            cJSON_Delete(item);
            cJSON_Delete(array);
            free(records);
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        cJSON_AddItemToArray(array, item);
    }
    free(records);
    char *encoded = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (!encoded) { err = ESP_ERR_NO_MEM; goto done; }
    size_t encoded_len = strlen(encoded);
    if (encoded_len + 1 > json_len) err = ESP_ERR_NO_MEM;
    else memcpy(json, encoded, encoded_len + 1);
    free(encoded);

done:
    xSemaphoreGive(s_config_lock);
    return err;
}
