#include "power_policy.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "power";

esp_err_t power_policy_init(void)
{
#if CONFIG_PM_ENABLE
    const esp_pm_config_t config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DFS configuration failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "DFS enabled CPU %d..%d MHz, light sleep disabled",
             CONFIG_XTAL_FREQ, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "PM disabled at build time; running fixed CPU clock");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void power_policy_status(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    wifi_ps_type_t ps;
    const char *ps_name = "unavailable";
    if (esp_wifi_get_ps(&ps) == ESP_OK) {
        switch (ps) {
        case WIFI_PS_NONE: ps_name = "NONE"; break;
        case WIFI_PS_MIN_MODEM: ps_name = "MIN_MODEM"; break;
        case WIFI_PS_MAX_MODEM: ps_name = "MAX_MODEM"; break;
        default: break;
        }
    }
#if CONFIG_PM_ENABLE
    snprintf(out, out_len, "dfs=on cpu=%uMHz range=%d..%dMHz apb=%uMHz wifi_ps=%s",
             (unsigned)(esp_clk_cpu_freq() / 1000000), CONFIG_XTAL_FREQ,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             (unsigned)(esp_clk_apb_freq() / 1000000), ps_name);
#else
    snprintf(out, out_len, "dfs=off cpu=%uMHz apb=%uMHz wifi_ps=%s",
             (unsigned)(esp_clk_cpu_freq() / 1000000),
             (unsigned)(esp_clk_apb_freq() / 1000000), ps_name);
#endif
}
