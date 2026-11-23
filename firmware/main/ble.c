#include "ble.h"
#include "rpc.h"
#include "cfg.h"
#include "ble_lifecycle_policy.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

/* NUS 风格 128bit UUID */
static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t CHR_CMD_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t CHR_RSP_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t s_cmd_handle;
static uint16_t s_rsp_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t s_conn_generation;
static char s_name[32];
static QueueHandle_t s_cmd_q;
typedef struct { char *json; uint16_t conn_handle; uint32_t generation; } ble_cmd_t;
static char s_rx[513];
static size_t s_rx_len;
static uint16_t s_rx_conn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_rx_generation;
static TaskHandle_t s_lifecycle_task_handle;
static TaskHandle_t s_cmd_task_handle;
static volatile bool s_host_running;
static bool s_nimble_host_stopped;
static unsigned s_cmd_pending;
static portMUX_TYPE s_lifecycle_mux = portMUX_INITIALIZER_UNLOCKED;

#define BLE_STOP_DRAIN_MS 30000
#define BLE_STOP_CONN_MS 2000

static bool ble_host_allowed(void)
{
    return ble_lifecycle_should_run(cfg_setup_done());
}

void ble_lockdown(void)
{
    if (s_lifecycle_task_handle) xTaskNotifyGive(s_lifecycle_task_handle);
}

static void advertise(void);

static bool notify_to(uint16_t conn_handle, uint32_t generation, const char *json)
{
    if (!ble_host_allowed() || conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_rsp_handle ||
        s_conn != conn_handle || s_conn_generation != generation) return false;
    size_t len = strlen(json), off = 0;
    uint16_t mtu = ble_att_mtu(conn_handle);
    size_t chunk = mtu > 3 ? mtu - 3 : 20;
    while (off < len) {
        if (!ble_host_allowed() || s_conn != conn_handle || s_conn_generation != generation) return false;
        size_t n = len - off;
        if (n > chunk) n = chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(json + off, n);
        if (!om) return false;
        int rc = ble_gatts_notify_custom(conn_handle, s_rsp_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify rc=%d", rc);
            return false;
        }
        off += n;
        if (off < len) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ble_host_allowed() || s_conn != conn_handle || s_conn_generation != generation) return false;
    struct os_mbuf *end = ble_hs_mbuf_from_flat("\n", 1);
    if (!end || ble_gatts_notify_custom(conn_handle, s_rsp_handle, end) != 0) return false;
    return true;
}

static bool command_pending_add(void)
{
    bool accepted;
    taskENTER_CRITICAL(&s_lifecycle_mux);
    accepted = ble_host_allowed();
    if (accepted) s_cmd_pending++;
    taskEXIT_CRITICAL(&s_lifecycle_mux);
    return accepted;
}

static void command_pending_done(void)
{
    taskENTER_CRITICAL(&s_lifecycle_mux);
    if (s_cmd_pending) s_cmd_pending--;
    taskEXIT_CRITICAL(&s_lifecycle_mux);
}

static void ble_cmd_task(void *arg)
{
    (void)arg;
    ble_cmd_t job;
    while (1) {
        if (xQueueReceive(s_cmd_q, &job, portMAX_DELAY) != pdTRUE || !job.json) continue;
        if (!ble_host_allowed()) {
            free(job.json);
            command_pending_done();
            continue;
        }
        char *response = malloc(4096);
        rpc_post_reply_action_t action = RPC_POST_NONE;
        if (response && rpc_dispatch_json(job.json, response, 4096, true, &action) == ESP_OK) {
            (void)notify_to(job.conn_handle, job.generation, response);
            rpc_run_post_reply_action(action);
        } else if (job.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            (void)notify_to(job.conn_handle, job.generation,
                            "{\"id\":0,\"ok\":false,\"err\":\"memory\"}");
        }
        free(response);
        free(job.json);
        command_pending_done();
    }
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (!ble_host_allowed()) return BLE_ATT_ERR_UNLIKELY;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (s_rx_len && (s_rx_conn != conn_handle || s_rx_generation != s_conn_generation)) {
            s_rx_len = 0;
            s_rx_conn = BLE_HS_CONN_HANDLE_NONE;
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_rx_conn = conn_handle;
        s_rx_generation = s_conn_generation;
        char buf[247];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        buf[len] = '\0';
        for (int i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                s_rx[s_rx_len] = 0;
                char *copy = strdup(s_rx);
                s_rx_len = 0;
                s_rx_conn = BLE_HS_CONN_HANDLE_NONE;
                if (!copy) return BLE_ATT_ERR_INSUFFICIENT_RES;
                ble_cmd_t job = { .json = copy, .conn_handle = conn_handle,
                                  .generation = s_conn_generation };
                if (!command_pending_add()) {
                    free(copy);
                    return BLE_ATT_ERR_UNLIKELY;
                }
                if (xQueueSend(s_cmd_q, &job, 0) != pdTRUE) {
                    free(copy);
                    command_pending_done();
                    return BLE_ATT_ERR_INSUFFICIENT_RES;
                }
            } else if (s_rx_len + 1 < sizeof(s_rx)) {
                s_rx_conn = conn_handle;
                s_rx[s_rx_len++] = buf[i];
            } else {
                s_rx_len = 0;
                s_rx_conn = BLE_HS_CONN_HANDLE_NONE;
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *p = "smsfwd";
        int rc = os_mbuf_append(ctxt->om, p, strlen(p));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    (void)conn_handle;
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &CHR_CMD_UUID.u,
                .access_cb = gatt_access,
                .val_handle = &s_cmd_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &CHR_RSP_UUID.u,
                .access_cb = gatt_access,
                .val_handle = &s_rsp_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    uint16_t svc_handle = 0, cmd_handle = 0, rsp_handle = 0;
    int find_svc = ble_gatts_find_svc(&SVC_UUID.u, &svc_handle);
    int find_cmd = ble_gatts_find_chr(&SVC_UUID.u, &CHR_CMD_UUID.u, NULL, &cmd_handle);
    int find_rsp = ble_gatts_find_chr(&SVC_UUID.u, &CHR_RSP_UUID.u, NULL, &rsp_handle);
    if (find_svc != 0 || find_cmd != 0 || find_rsp != 0 || !s_cmd_handle || !s_rsp_handle ||
        cmd_handle != s_cmd_handle || rsp_handle != s_rsp_handle) {
        ESP_LOGE(TAG, "GATT discovery check failed svc=%d cmd=%d rsp=%d handles=%u/%u/%u",
                 find_svc, find_cmd, find_rsp, svc_handle, cmd_handle, rsp_handle);
        return;
    }
    ESP_LOGI(TAG, "custom GATT ready svc=%u cmd=%u rsp=%u", svc_handle, cmd_handle, rsp_handle);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_conn = (event->connect.status == 0) ? event->connect.conn_handle : BLE_HS_CONN_HANDLE_NONE;
        s_conn_generation++;
        ESP_LOGI(TAG, "connect status=%d", event->connect.status);
        if (!ble_host_allowed() && s_conn != BLE_HS_CONN_HANDLE_NONE) {
            (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        } else if (event->connect.status != 0) advertise();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_conn_generation++;
        s_rx_len = 0;
        s_rx_conn = BLE_HS_CONN_HANDLE_NONE;
        s_rx_generation = s_conn_generation;
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        if (ble_host_allowed()) advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (ble_host_allowed()) advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (!ble_host_allowed()) return;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_name;
    fields.name_len = strlen(s_name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }
    struct ble_hs_adv_fields scan_rsp = {0};
    scan_rsp.uuids128 = &SVC_UUID;
    scan_rsp.num_uuids128 = 1;
    scan_rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_rsp);
    if (rc != 0) { ESP_LOGE(TAG, "scan response rc=%d", rc); return; }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.channel_map = 0x07;
    params.itvl_min = 0x20;
    params.itvl_max = 0x30;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv start rc=%d", rc);
}

static esp_err_t ble_host_start(void)
{
    if (s_host_running) return ESP_OK;
    s_nimble_host_stopped = false;

    esp_err_t err = rpc_init();
    if (err != ESP_OK) return err;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "SMSF-%02X%02X", mac[4], mac[5]);

    s_cmd_q = xQueueCreate(4, sizeof(ble_cmd_t));
    if (!s_cmd_q) return ESP_ERR_NO_MEM;
    err = nimble_port_init();
    if (err != ESP_OK) goto fail_queue;

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(247);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(GATT_SERVICES);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT count_cfg rc=%d", rc);
        err = ESP_FAIL;
        goto fail_nimble;
    }
    rc = ble_gatts_add_svcs(GATT_SERVICES);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT add_svcs rc=%d", rc);
        err = ESP_FAIL;
        goto fail_nimble;
    }
    ble_svc_gap_device_name_set(s_name);

    if (xTaskCreate(ble_cmd_task, "ble_cmd", 6144, NULL, 5,
                    &s_cmd_task_handle) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail_nimble;
    }

    nimble_port_freertos_init(host_task);
    s_host_running = true;
    ESP_LOGI(TAG, "advertising as %s", s_name);
    return ESP_OK;

fail_nimble:
    (void)nimble_port_deinit();
fail_queue:
    vQueueDelete(s_cmd_q);
    s_cmd_q = NULL;
    return err;
}

static esp_err_t ble_host_stop(void)
{
    if (!s_host_running) return ESP_OK;

    (void)ble_gap_adv_stop();

    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        int64_t deadline = esp_timer_get_time() / 1000 + BLE_STOP_CONN_MS;
        while (s_conn != BLE_HS_CONN_HANDLE_NONE && esp_timer_get_time() / 1000 < deadline)
            vTaskDelay(pdMS_TO_TICKS(20));
    }

    int64_t deadline = esp_timer_get_time() / 1000 + BLE_STOP_DRAIN_MS;
    for (;;) {
        taskENTER_CRITICAL(&s_lifecycle_mux);
        unsigned pending = s_cmd_pending;
        taskEXIT_CRITICAL(&s_lifecycle_mux);
        if (pending == 0) break;
        if (esp_timer_get_time() / 1000 >= deadline) {
            ESP_LOGW(TAG, "waiting for %u BLE RPC(s) before host shutdown", pending);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_cmd_task_handle) {
        vTaskDelete(s_cmd_task_handle);
        s_cmd_task_handle = NULL;
    }
    if (s_cmd_q) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
    }

    if (!s_nimble_host_stopped) {
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "NimBLE host stop failed: %d", rc);
            return ESP_FAIL;
        }
        s_nimble_host_stopped = true;
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE controller deinit failed: %s", esp_err_to_name(err));
        return err;
    }

    s_host_running = false;
    s_nimble_host_stopped = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_cmd_handle = s_rsp_handle = 0;
    s_rx_len = 0;
    s_rx_conn = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "NimBLE host and controller released");
    return ESP_OK;
}

static void ble_lifecycle_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool should_run = ble_host_allowed();

        bool retry = false;
        if (should_run && !s_host_running) {
            esp_err_t err = ble_host_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
                retry = true;
            }
        } else if (!should_run && s_host_running) {
            esp_err_t err = ble_host_stop();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "BLE shutdown deferred: %s", esp_err_to_name(err));
                retry = true;
            }
        }

        (void)ulTaskNotifyTake(pdTRUE, retry ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
    }
}

esp_err_t ble_start(void)
{
    esp_err_t rpc_err = rpc_init();
    if (rpc_err != ESP_OK) return rpc_err;

    if (xTaskCreate(ble_lifecycle_task, "ble_lifecycle", 4096, NULL, 4,
                    &s_lifecycle_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "BLE lifecycle task creation failed");
        s_lifecycle_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_lifecycle_task_handle);
    return ESP_OK;
}

bool ble_connected(void)
{
    return ble_host_allowed() && s_conn != BLE_HS_CONN_HANDLE_NONE;
}
