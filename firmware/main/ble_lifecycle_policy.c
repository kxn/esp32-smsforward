#include "ble_lifecycle_policy.h"

bool ble_lifecycle_should_run(bool setup_done)
{
    return !setup_done;
}

bool ble_lifecycle_can_complete(bool setup_done, bool wifi_has_ip,
                                bool feishu_ws_online)
{
    return !setup_done && wifi_has_ip && feishu_ws_online;
}
