#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cfg.h"

esp_err_t wifi_start(const app_cfg_t *cfg);
esp_err_t wifi_reconfigure(const app_cfg_t *cfg);
esp_err_t wifi_preinit(void);   /* Initialize network interfaces without starting Wi-Fi. */
bool wifi_config_applied(void);
bool wifi_connected(void);
bool wifi_online_for(const app_cfg_t *cfg); /* Got IPv4 on the currently saved AP. */
uint32_t wifi_uptime_seconds(void);
void wifi_status(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi);
esp_err_t wifi_scan(char *json, size_t json_len);

esp_err_t sntp_start(void);
bool time_synced(void);
