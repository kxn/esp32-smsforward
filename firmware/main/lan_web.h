#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Start/stop the LAN management server with Wi-Fi connectivity. */
esp_err_t lan_web_start(void);
void lan_web_stop(void);

/* Create or reuse the current boot's private management link. Returns false without IPv4. */
bool lan_web_management_url(char *out, size_t cap);
