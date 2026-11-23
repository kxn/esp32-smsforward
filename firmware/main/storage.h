#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t storage_mount(void);
bool storage_ready(void);
