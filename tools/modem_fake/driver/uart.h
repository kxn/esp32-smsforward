#pragma once
#include <stddef.h>
#include <stdint.h>
#include "freertos/queue.h"
typedef int uart_port_t;
typedef struct { int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk; } uart_config_t;
typedef struct { enum { UART_DATA, UART_FIFO_OVF } type; size_t size; } uart_event_t;
#define UART_DATA_8_BITS 0
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 0
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_SCLK_XTAL 1
#define UART_PIN_NO_CHANGE (-1)
int uart_read_bytes(uart_port_t uart, void *buf, uint32_t len, uint32_t ticks);
int uart_write_bytes(uart_port_t uart, const void *buf, size_t len);
int uart_driver_install(uart_port_t uart, int rx, int tx, int events, QueueHandle_t *queue, int flags);
int uart_param_config(uart_port_t uart, const uart_config_t *cfg);
int uart_set_pin(uart_port_t uart, int tx, int rx, int rts, int cts);
int uart_flush_input(uart_port_t uart);
