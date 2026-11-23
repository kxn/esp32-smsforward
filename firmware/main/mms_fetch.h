#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "modem.h"

/* Retrieve a validated MMS HTTP body using only the modem's cellular bearer.
 * On success, *body is heap allocated and owned by the caller. */
esp_err_t mms_fetch(modem_t *modem, const char *url, uint8_t **body, size_t *len);
