#pragma once

#include <stdbool.h>
#include "esp_err.h"

bool factory_reset_pending(void);
esp_err_t factory_reset_request(void);

/* Call after NVS init and before loading configuration, mounting storage or
 * starting services. On failure, retry recovery before serving requests. */
esp_err_t factory_reset_recover(void);

/* The journal is cleared only after every idempotent erase has succeeded. */
typedef struct {
    esp_err_t (*erase_config)(void);
    esp_err_t (*restore_wifi)(void);
    esp_err_t (*erase_storage)(void);
    esp_err_t (*clear_pending)(void);
} factory_reset_steps_t;

esp_err_t factory_reset_apply(bool pending, const factory_reset_steps_t *steps);
