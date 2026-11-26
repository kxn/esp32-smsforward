#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    bool dhcp;
    char ip[16];
    char mask[16];
    char gw[16];
    char dns[16];

    char app_id[64];
    char app_secret[128];
    char open_id[64];
    char chat_id[64];

    char slot_name[2][24];
    char slot_num[2][32];

    int8_t tz_offset;      /* 小时，默认 8 */

    /* Optional online mark lookup for incoming calls; empty URL disables it. */
    char mark_url[192];
    char mark_key[64];
    uint16_t mark_timeout_ms;
} app_cfg_t;

/* Bounds for the optional online mark lookup budget. */
#define MARK_TIMEOUT_DEFAULT_MS 2000
#define MARK_TIMEOUT_MIN_MS 500
#define MARK_TIMEOUT_MAX_MS 5000

esp_err_t cfg_init(void);
const app_cfg_t *cfg_get(void);
app_cfg_t *cfg_mut(void);
esp_err_t cfg_save(void);
bool cfg_setup_done(void);
esp_err_t cfg_read_setup_credentials(app_cfg_t *out);
/* Commit only when the already checked connection uses the saved credentials. */
esp_err_t cfg_commit_setup(const app_cfg_t *expected);
/* Startup reset recovery only. */
esp_err_t cfg_erase_persisted(void);
void cfg_apply_timezone(void);
void cfg_defaults(void);
