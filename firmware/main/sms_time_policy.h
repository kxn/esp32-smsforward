#pragma once

#include <stdbool.h>
#include <time.h>

/* Prefer the SMS's stored absolute receive time. Fallbacks retain the existing
 * clock-sync marker behavior for the current device clock or modem clock. */
static inline time_t sms_time_choose(time_t scts, bool device_synced,
                                     time_t device_now, time_t modem_now,
                                     bool *synced)
{
    if (scts > 0) {
        if (synced) *synced = true;
        return scts;
    }
    if (device_synced) {
        if (synced) *synced = true;
        return device_now;
    }
    if (synced) *synced = false;
    return modem_now > 0 ? modem_now : device_now;
}
