#include <assert.h>
#include <stdio.h>

#include "ble_lifecycle_policy.h"

int main(void)
{
    /* Fresh and legacy devices remain provisionable until both are online. */
    assert(ble_lifecycle_should_run(false));
    assert(!ble_lifecycle_can_complete(false, false, false));
    assert(!ble_lifecycle_can_complete(false, true, false));
    assert(!ble_lifecycle_can_complete(false, false, true));
    assert(ble_lifecycle_can_complete(false, true, true));

    /* Persisted completion keeps BLE off even if either service goes offline. */
    assert(!ble_lifecycle_should_run(true));
    assert(!ble_lifecycle_can_complete(true, false, false));
    assert(!ble_lifecycle_can_complete(true, true, true));

    puts("BLE setup lifecycle policy passed");
    return 0;
}
