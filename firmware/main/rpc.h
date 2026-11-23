#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    RPC_POST_NONE = 0,
    RPC_POST_WIFI_RECONFIGURE,
    RPC_POST_RESTART,
} rpc_post_reply_action_t;

esp_err_t rpc_init(void);
/* Called by the network state machine. True only after durable setup completion. */
bool rpc_complete_setup(void);
/* Dispatch one BLE/LAN JSON request into a one-line JSON response. */
esp_err_t rpc_dispatch_json(const char *request, char *response, size_t response_len,
                            bool via_ble,
                            rpc_post_reply_action_t *post_action);
/* Call only after the transport has sent the response. */
void rpc_run_post_reply_action(rpc_post_reply_action_t action);
