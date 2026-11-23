#pragma once
#include "freertos/FreeRTOS.h"

typedef struct fake_queue *QueueHandle_t;
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
