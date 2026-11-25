#pragma once
/* Host shim for the ESP-IDF heap capability API: the host has one heap, so every
 * capability collapses to malloc and the accounting calls report nothing. */
#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_8BIT (1u << 2)

static inline void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

static inline size_t heap_caps_get_free_size(unsigned caps)
{
    (void)caps;
    return 0;
}

static inline size_t heap_caps_get_total_size(unsigned caps)
{
    (void)caps;
    return 0;
}
