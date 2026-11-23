#include "health.h"
#include "board.h"
#include "sms.h"
#include "cfg.h"
#include "feishu.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "health";

/* 原厂固件策略：5 分钟 AT 探活，2s 超时，连续 3 次无响应 -> GPIO 复位 */
#define PROBE_INTERVAL_MS (5 * 60 * 1000)
#define PROBE_TIMEOUT_MS  2000
#define PROBE_THRESHOLD   3

static modem_t *s_modems;
static int s_fail[2];
static volatile int s_blink_sms;
static volatile int s_blink_err;

void health_sms_blink(void) { s_blink_sms = 1; }
void health_error_blink(void) { s_blink_err = 2; }

static void reinit_modem(modem_t *m)
{
    /* Reset clears the modem transaction boundary; reapply SMS state for this slot. */
    modem_reset_hw(m);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_err_t err = sms_modem_configure(m->idx);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "%s reconfigured after reset", modem_name(m));
    else
        ESP_LOGW(TAG, "%s SMS reconfiguration pending retry (%d)", modem_name(m), (int)err);
}

static void health_task(void *arg)
{
    (void)arg;
    int64_t next_probe = esp_timer_get_time() / 1000 + 30000;

    while (1) {
        int64_t now = esp_timer_get_time() / 1000;

        /* 探活 */
        if (now >= next_probe) {
            next_probe = now + PROBE_INTERVAL_MS;
            for (int i = 0; i < MODEM_COUNT; i++) {
                esp_err_t e = modem_cmd(&s_modems[i], "AT", NULL, 0, PROBE_TIMEOUT_MS);
                if (e == ESP_OK) {
                    if (s_fail[i]) {
                        ESP_LOGI(TAG, "%s recovered", modem_name(&s_modems[i]));
                    }
                    s_fail[i] = 0;
                } else if (e == ESP_ERR_TIMEOUT || e == MODEM_ERR_DESYNC) {
                    s_fail[i]++;
                    ESP_LOGW(TAG, "%s no response (%d/%d)", modem_name(&s_modems[i]),
                             s_fail[i], PROBE_THRESHOLD);
                    if (s_fail[i] >= PROBE_THRESHOLD) {
                        s_fail[i] = 0;
                        ESP_LOGE(TAG, "%s dead, hardware reset", modem_name(&s_modems[i]));
                        reinit_modem(&s_modems[i]);
                    }
                } else if (e != MODEM_ERR_BUSY) {
                    /* An explicit modem error still proves that the UART is live. */
                    s_fail[i] = 0;
                    ESP_LOGW(TAG, "%s probe returned error %d", modem_name(&s_modems[i]), (int)e);
                }
                if (!sms_modem_is_configured(i)) {
                    esp_err_t cfg_err = sms_modem_configure(i);
                    if (cfg_err != ESP_OK)
                        ESP_LOGW(TAG, "%s SMS setup retry failed (%d)",
                                 modem_name(&s_modems[i]), (int)cfg_err);
                }
            }
        }

        /* LED */
        static int tick;
        tick++;
        bool fs_ok = feishu_ws_connected();

        gpio_set_level(PIN_LED1, fs_ok ? 1 : (tick % 4 < 2));
        if (s_blink_sms > 0) {
            gpio_set_level(PIN_LED2, 1);
            s_blink_sms--;
        } else if (s_blink_err > 0) {
            gpio_set_level(PIN_LED2, tick % 2);
            s_blink_err--;
        } else {
            gpio_set_level(PIN_LED2, 0);
        }

        /* LED state does not need a 4 Hz polling task. UART URCs are handled
         * by the independent RX workers, not by this heartbeat. */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void health_start(modem_t *modems)
{
    s_modems = modems;
    memset(s_fail, 0, sizeof(s_fail));
    s_blink_sms = 0;
    s_blink_err = 0;
    xTaskCreate(health_task, "health", 4096, NULL, 3, NULL);
}
