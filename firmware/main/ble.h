#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ble_start(void);
bool ble_connected(void);
void ble_lockdown(void); /* Shut down BLE after persisting setup completion. */
