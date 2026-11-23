#pragma once

#include <stdbool.h>

/* Legacy/unconfigured devices keep BLE available until both services are
 * proven. A physical window may temporarily reopen BLE after completion. */
bool ble_lifecycle_should_run(bool setup_done, bool physical_window_active);
bool ble_lifecycle_can_complete(bool setup_done, bool wifi_has_ip,
                               bool feishu_ws_online);
