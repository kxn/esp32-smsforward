#pragma once

#include <stdbool.h>

/* BLE is available until the device persists setup completion. */
bool ble_lifecycle_should_run(bool setup_done);
bool ble_lifecycle_can_complete(bool setup_done, bool wifi_has_ip,
                               bool feishu_ws_online);
