#include "ble.h"
#include "rpc.h"
#include "board.h"
#include "driver/gpio.h"

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
static volatile int64_t s_provision_until_ms;
static volatile int64_t s_ble_last_activity_ms;

#define BLE_IDLE_TIMEOUT_MS 45000

static void ble_idle_watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        uint16_t conn = s_conn;
        if (conn != BLE_HS_CONN_HANDLE_NONE && s_ble_last_activity_ms > 0 &&
            now - s_ble_last_activity_ms > BLE_IDLE_TIMEOUT_MS) {
            if (ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM) == 0) {
                ESP_LOGI(TAG, "closed idle BLE connection");
                s_ble_last_activity_ms = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

bool ble_physical_presence_active(void)
{
    return esp_timer_get_time() / 1000 < s_provision_until_ms;
}

void ble_provision_open(void)
{
    s_provision_until_ms = esp_timer_get_time() / 1000 + 120000;
    ESP_LOGI(TAG, "local provisioning window opened");
}

static void boot_button_task(void *arg)
{
    (void)arg;
    bool was_down = false;
    for (;;) {
        bool down = gpio_get_level(PIN_BOOT) == 0;
        if (down && !was_down) {
            ble_provision_open();
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static void advertise(void);

static bool notify_to(uint16_t conn_handle, uint32_t generation, const char *json)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_rsp_handle ||
        s_conn != conn_handle || s_conn_generation != generation) return false;
    size_t len = strlen(json), off = 0;
    uint16_t mtu = ble_att_mtu(conn_handle);
    size_t chunk = mtu > 3 ? mtu - 3 : 20;
    while (off < len) {
        if (s_conn != conn_handle || s_conn_generation != generation) return false;
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
    if (s_conn != conn_handle || s_conn_generation != generation) return false;
    struct os_mbuf *end = ble_hs_mbuf_from_flat("\n", 1);
    if (!end || ble_gatts_notify_custom(conn_handle, s_rsp_handle, end) != 0) return false;
    return true;
}

static void ble_cmd_task(void *arg)
{
    (void)arg;
    ble_cmd_t job;
    while (1) {
        if (xQueueReceive(s_cmd_q, &job, portMAX_DELAY) != pdTRUE || !job.json) continue;
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
    }
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        s_ble_last_activity_ms = esp_timer_get_time() / 1000;
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
                if (xQueueSend(s_cmd_q, &job, 0) != pdTRUE) {
                    free(copy);
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
        s_ble_last_activity_ms = esp_timer_get_time() / 1000;
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
        s_ble_last_activity_ms = (s_conn != BLE_HS_CONN_HANDLE_NONE) ? esp_timer_get_time() / 1000 : 0;
        ESP_LOGI(TAG, "connect status=%d", event->connect.status);
        if (event->connect.status != 0) advertise();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_conn_generation++;
        s_ble_last_activity_ms = 0;
        s_rx_len = 0;
        s_rx_conn = BLE_HS_CONN_HANDLE_NONE;
        s_rx_generation = s_conn_generation;
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        s_ble_last_activity_ms = esp_timer_get_time() / 1000;
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
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

esp_err_t ble_start(void)
{
    esp_err_t rpc_err = rpc_init();
    if (rpc_err != ESP_OK) return rpc_err;
    gpio_config_t boot = { .pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&boot);
    xTaskCreate(boot_button_task, "ble_button", 2048, NULL, 3, NULL);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "SMSF-%02X%02X", mac[4], mac[5]);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(247);

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(GATT_SERVICES);
    if (rc != 0) { ESP_LOGE(TAG, "GATT count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(GATT_SERVICES);
    if (rc != 0) { ESP_LOGE(TAG, "GATT add_svcs rc=%d", rc); return ESP_FAIL; }
    ble_svc_gap_device_name_set(s_name);
    s_cmd_q = xQueueCreate(4, sizeof(ble_cmd_t));
    if (!s_cmd_q) return ESP_ERR_NO_MEM;
    xTaskCreate(ble_cmd_task, "ble_cmd", 6144, NULL, 5, NULL);
    xTaskCreate(ble_idle_watchdog, "ble_idle", 2048, NULL, 3, NULL);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "advertising as %s", s_name);
    return ESP_OK;
}

bool ble_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}
