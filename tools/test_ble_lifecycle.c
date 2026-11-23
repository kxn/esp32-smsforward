#include <assert.h>
#include <stdio.h>

#include "ble_lifecycle_policy.h"

int main(void)
{
    /* Fresh and legacy devices remain provisionable until both are online. */
    assert(ble_lifecycle_should_run(false, false));
    assert(ble_lifecycle_can_complete(false, false, false) == false);
    assert(ble_lifecycle_can_complete(false, true, false) == false);
    assert(ble_lifecycle_can_complete(false, false, true) == false);
    assert(ble_lifecycle_can_complete(false, true, true));

    /* Completed devices stay off across outages/reboots, unless physical
     * presence explicitly opens the recovery window. */
    assert(!ble_lifecycle_should_run(true, false));
    assert(!ble_lifecycle_can_complete(true, true, true));
    assert(ble_lifecycle_should_run(true, true));

    puts("BLE setup completion and physical recovery policy passed");
    return 0;
}
