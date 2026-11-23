#include "mms_inbox.h"
#include "mms_fetch.h"
#include "cmd.h"
#include "feishu.h"
#include "wap_mms.h"
#include "wifi.h"
#include "sms.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    modem_t *modem;
    char url[WAP_MMS_LOCATION_MAX];
    char parent[64];
    char record_id[40];
    uint64_t key;
    bool (*commit)(uint64_t);
} mms_job_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_active;

static bool original_sim(const mms_job_t *j)
{
    int slot = -1;
    char expected[24] = "", current[24] = "";
    return map_lookup_sim(j->parent, &slot, NULL, 0, NULL, 0, expected, sizeof(expected)) &&
           slot == j->modem->idx && expected[0] && cmd_current_iccid(slot, current) &&
           !strcmp(expected, current);
}

static esp_err_t forward_parts(const mms_job_t *j, const wap_mms_retrieve_t *pdu)
{
    feishu_media_part_t parts[FEISHU_MEDIA_PARTS_MAX] = {0};
    if (!pdu->part_count) return ESP_OK;
    if (pdu->part_count > FEISHU_MEDIA_PARTS_MAX) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < pdu->part_count; ++i) {
        const wap_mms_part_t *p = &pdu->parts[i];
        parts[i] = (feishu_media_part_t){ .mime_type = p->mime, .charset = p->charset,
            .filename = p->filename, .data = p->data, .size = p->len };
    }
    /* Feishu's UUIDs stay stable across partial sends. The retry uses the same
     * downloaded bytes, without reopening a cellular bearer. */
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3; ++i) {
        feishu_media_result_t result = {0};
        err = feishu_send_media_parts(j->parent, parts, pdu->part_count, j->record_id, &result);
        for (size_t k = 0; k < result.sent_count; ++k)
            if (!map_note_child(j->parent, result.message_ids[k])) err = ESP_FAIL;
        if (err == ESP_OK || i == 2) break;
        vTaskDelay(pdMS_TO_TICKS(i == 0 ? 2000 : 8000));
    }
    return err;
}

static void process(void *arg)
{
    mms_job_t *j = arg;
    uint8_t *body = NULL;
    size_t len = 0;
    /* Prefer to leave the item on the SIM if Wi-Fi or identity verification is
     * temporarily unavailable before the first cellular operation. */
    bool attempted = wifi_connected() && feishu_ws_connected() && original_sim(j);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (attempted) {
        err = mms_fetch(j->modem, j->url, &body, &len);
        if (err == ESP_OK) {
            wap_mms_retrieve_t *pdu = heap_caps_malloc(sizeof(*pdu), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!pdu) err = ESP_ERR_NO_MEM;
            else if (wap_mms_parse_retrieve_mutable(body, len, pdu) != WAP_MMS_OK) err = ESP_ERR_INVALID_RESPONSE;
            else err = forward_parts(j, pdu);
            free(pdu);
        }
        /* A notification has already reached the owner. Bound automatic data
         * usage to one retrieval instead of refetching on every SIM sweep. */
        if (!j->commit(j->key)) ESP_LOGW("mms", "completion journal unavailable");
    }
    if (attempted && err != ESP_OK)
        ESP_LOGI("mms", "notification retained; content unavailable (%d)", (int)err);
    free(body);
    free(j);
    taskENTER_CRITICAL(&s_mux);
    s_active = false;
    taskEXIT_CRITICAL(&s_mux);
    if (attempted) sms_retry_pending();
    vTaskDelete(NULL);
}

void mms_inbox_schedule(modem_t *m, const char *url, const char *parent,
                        const char *record_id, uint64_t key, bool (*commit)(uint64_t))
{
    if (!wifi_connected() || !feishu_ws_connected()) return;
    taskENTER_CRITICAL(&s_mux);
    bool start = !s_active;
    if (start) s_active = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!start) return;

    mms_job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (j) {
        j->modem = m;
        j->key = key;
        j->commit = commit;
        strlcpy(j->url, url, sizeof(j->url));
        strlcpy(j->parent, parent, sizeof(j->parent));
        strlcpy(j->record_id, record_id, sizeof(j->record_id));
        if (xTaskCreate(process, "mms", 14336, j, 4, NULL) == pdPASS) return;
        free(j);
    }
    taskENTER_CRITICAL(&s_mux);
    s_active = false;
    taskEXIT_CRITICAL(&s_mux);
}
