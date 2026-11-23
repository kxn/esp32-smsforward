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
} app_cfg_t;

esp_err_t cfg_init(void);
const app_cfg_t *cfg_get(void);
app_cfg_t *cfg_mut(void);
esp_err_t cfg_save(void);
void cfg_apply_timezone(void);
void cfg_defaults(void);
