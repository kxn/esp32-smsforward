#pragma once
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_ERROR_CHECK(expr) do { (void)(expr); } while (0)
static inline size_t fake_strlcpy(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n) { size_t copy = len < n - 1 ? len : n - 1; memcpy(dst, src, copy); dst[copy] = '\0'; }
    return len;
}
#define strlcpy fake_strlcpy
