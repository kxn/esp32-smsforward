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

/* Recovery primitives used by the connection supervisor.  All of them are
 * best-effort and safe to call while the device is running. */
esp_err_t time_sync_restart(void);
esp_err_t wifi_dhcp_renew(void);
esp_err_t wifi_reassociate(void);

/* Probe whether the internet (not just the AP link) is reachable: resolves
 * open.feishu.cn and opens a TCP connection to its 443 port.  Either output may
 * be NULL.  Returns true only when both steps succeeded. */
bool wifi_probe_internet(bool *dns_ok, bool *tcp_ok, int timeout_ms);
