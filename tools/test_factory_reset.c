#include "factory_reset.h"

#include <assert.h>
#include <stdio.h>

static unsigned fail_at;
static unsigned calls;
static unsigned erased;
static bool pending = true;

static esp_err_t erase_at(unsigned step)
{
    calls++;
    erased |= 1u << step;
    /* Mimic losing power after a destructive step has partly executed. */
    return fail_at == step ? ESP_FAIL : ESP_OK;
}

static esp_err_t erase_cfg(void) { return erase_at(0); }
static esp_err_t erase_wifi(void) { return erase_at(1); }
static esp_err_t erase_fat(void) { return erase_at(2); }

static esp_err_t clear_pending(void)
{
    calls++;
    assert(erased == 7);
    if (fail_at == 3) return ESP_FAIL;
    pending = false;
    return ESP_OK;
}

int main(void)
{
    const factory_reset_steps_t steps = { erase_cfg, erase_wifi, erase_fat, clear_pending };
    for (unsigned interruption = 0; interruption < 4; ++interruption) {
        fail_at = interruption;
        calls = erased = 0;
        pending = true;
        assert(factory_reset_apply(pending, &steps) == ESP_FAIL);
        assert(pending && calls == interruption + 1);

        /* Reboot retains only the pending journal and persistent effects. */
        fail_at = 4;
        calls = 0;
        assert(factory_reset_apply(pending, &steps) == ESP_OK);
        assert(!pending && erased == 7 && calls == 4);
        calls = 0;
        assert(factory_reset_apply(pending, &steps) == ESP_OK && calls == 0);
    }
    puts("Factory reset journal survives interruptions until all erases finish");
    return 0;
}
