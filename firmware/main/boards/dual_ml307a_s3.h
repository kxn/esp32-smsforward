#pragma once

#define MODEM_COUNT 2

static const modem_pin_t MODEM_PINS[MODEM_COUNT] = {
    { UART_NUM_0, 43, 44, 9,  "M1" },
    { UART_NUM_1, 17, 18, 21, "M2" },
};


/* LED */
#define PIN_LED1 38
#define PIN_LED2 15
#define PIN_BOOT 0
