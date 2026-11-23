#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

/* 官方引脚表 + 原厂固件模块表一致 */
typedef struct {
    uart_port_t uart;
    int tx;
    int rx;
    int rst;
    const char *name;
} modem_pin_t;

#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "boards/dual_ml307a_s3.h"
#else
#error "Select a verified board pin map before building for this target."
#endif
