#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Enables CPU DFS from the configured CPU maximum down to the 40 MHz XTAL.
 * Automatic light sleep stays disabled so UART/USB/BLE service remains live. */
esp_err_t power_policy_init(void);

/* Report current CPU/APB clocks for diagnostics and board acceptance logs. */
void power_policy_status(char *out, size_t out_len);
