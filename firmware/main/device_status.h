#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;
    bool registered;
    int csq;
    char operator_name[32];
} device_slot_status_t;

extern device_slot_status_t device_slots[2];
void device_slot_config_changed(int slot, const char *name, const char *number);
