#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ble_start(void);
bool ble_connected(void);
void ble_provision_open(void); /* Trusted local USB command opens a 120s config window. */
bool ble_physical_presence_active(void);
void ble_lockdown(void); /* Permanently disable until a physical/USB recovery window. */
