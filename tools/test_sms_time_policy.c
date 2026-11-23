#include <assert.h>
#include <stdio.h>
#include "../firmware/main/sms_time_policy.h"

int main(void)
{
    const time_t received = (time_t)1795360350; /* stored 2026-11-22 SCTS */
    const time_t delayed_delivery = (time_t)1890000000;
    bool synced = false;
    time_t shown = sms_time_choose(received, true, delayed_delivery, 0, &synced);
    assert(shown == received && synced); /* retry shows original receive time */

    shown = sms_time_choose(0, true, delayed_delivery, 0, &synced);
    assert(shown == delayed_delivery && synced); /* no SCTS: device clock */

    const time_t modem_clock = (time_t)1780000000;
    shown = sms_time_choose(0, false, delayed_delivery, modem_clock, &synced);
    assert(shown == modem_clock && !synced); /* no SCTS/NTIME: modem fallback marker */
    puts("SMS received-time selection passed");
    return 0;
}
