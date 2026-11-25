#include "feishu.h"
#include "feishu_config_policy.h"
#include "pbbp2.h"
#include "feishu_proto.h"
#include "feishu_event_dedup.h"
#include "feishu_retry_policy.h"
#include "feishu_auth_policy.h"
#include "feishu_media_policy.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "feishu";

#define API_BASE "https://open.feishu.cn"
#define TOKEN_PATH "/open-apis/auth/v3/tenant_access_token/internal"
#define MSG_PATH "/open-apis/im/v1/messages"
#define WS_ENDPOINT_PATH "/callback/ws/endpoint"

static feishu_cfg_t s_cfg;
static bool s_cfg_initialized;
static volatile uint32_t s_cfg_generation;
static char s_token[256];
static int64_t s_token_expire_ms;
/* Bumped whenever the configuration changes.  token_get() treats a token that
 * was filled for an older epoch as absent, which is how feishu_init() invalidates
 * the cache without ever taking s_token_lock (see feishu_init()). */
static volatile uint32_t s_token_epoch;
static uint32_t s_token_fill_epoch;    /* epoch s_token was fetched for; s_token_lock */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_ws_lock;
static SemaphoreHandle_t s_ws_lifecycle_lock;
static feishu_cmd_cb_t s_cmd_cb;
static feishu_peer_cb_t s_peer_cb;

static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_connected;
static volatile int s_ws_reconnects;
/* Epoch of the last session already counted in s_ws_reconnects: ERROR,
 * DISCONNECTED, CLOSED and FINISH can all describe the same dead session, so the
 * counter advances once per session instead of once per event. */
static volatile uint32_t s_ws_counted_epoch;
static int s_ping_interval = 120;
static char s_ws_url[512];
static int s_service_id;
static char s_conn_id[128];
static TaskHandle_t s_ping_task;
static int64_t s_ws_created_ms;
static int64_t s_ws_retry_at_ms;
static volatile uint32_t s_ws_epoch;
static volatile uint32_t s_ws_cfg_generation;
static volatile uint32_t s_ws_online_epoch;
/* Liveness bookkeeping for the long connection.  The two 64-bit stamps are
 * written from the WebSocket task (event handler) and read from net_task, the
 * health task and rpc, so they are copied under a tiny critical section: 32-bit
 * Xtensa has no atomic 64-bit load/store and a torn read would flip a healthy
 * session to "dead".  They are NOT written under s_ws_lock because the event
 * handler must never block behind a send. */
static SemaphoreHandle_t s_token_lock;
static portMUX_TYPE s_ws_liveness_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_ws_connected_ms;    /* when CONNECTED arrived */
static int64_t s_ws_last_ctrl_ms;    /* last frame of any kind, incl. WS ping/pong */
static int64_t s_ws_last_app_ms;     /* last application frame (pong or event) */
static volatile bool s_ws_forced_dead;
/* Consecutive REST failures, written by the two send paths in this file and read
 * by sms.c (to park a delivery) and rpc.c (status page); the health supervisor
 * does not consult it. */
static volatile uint32_t s_send_fail_streak;

#define WS_RECREATE_MIN_AGE_MS 20000

static void ws_epoch_advance(void)
{
    s_ws_epoch++;
    if (!s_ws_epoch) s_ws_epoch++;
}

/* 64-bit, read under s_ws_lock by feishu_ws_start(): writes go through here so
 * the fetch-failure paths (which run without any session lock) stay consistent. */
static void ws_retry_later(int64_t delay_ms)
{
    int64_t at = esp_timer_get_time() / 1000 + delay_ms;
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    s_ws_retry_at_ms = at;
    xSemaphoreGive(s_ws_lock);
}

/* ---------------- long-connection liveness ---------------- */

static void ws_liveness_reset(void)
{
    portENTER_CRITICAL(&s_ws_liveness_mux);
    s_ws_connected_ms = 0;
    s_ws_last_ctrl_ms = 0;
    s_ws_last_app_ms = 0;
    portEXIT_CRITICAL(&s_ws_liveness_mux);
}

static void ws_liveness_note_connected(int64_t now_ms)
{
    portENTER_CRITICAL(&s_ws_liveness_mux);
    s_ws_connected_ms = now_ms;
    s_ws_last_ctrl_ms = now_ms;
    /* The application clock restarts with the session: keeping the previous
     * session's stamp would make a brand new connection look stale before its
     * first pong could arrive. */
    s_ws_last_app_ms = 0;
    portEXIT_CRITICAL(&s_ws_liveness_mux);
}

/* Any frame at all: proves the TCP path carries bytes, nothing more. */
static void ws_liveness_note_ctrl(int64_t now_ms)
{
    portENTER_CRITICAL(&s_ws_liveness_mux);
    s_ws_last_ctrl_ms = now_ms;
    portEXIT_CRITICAL(&s_ws_liveness_mux);
}

/* An application-level frame: the answer to our application ping or a delivered
 * event.  This is the only signal that says the *Feishu session* works. */
static void ws_liveness_note_app(int64_t now_ms)
{
    portENTER_CRITICAL(&s_ws_liveness_mux);
    s_ws_last_app_ms = now_ms;
    portEXIT_CRITICAL(&s_ws_liveness_mux);
}

/* Reference instant for application freshness.  Before the first application
 * frame arrives, the CONNECTED instant is the evidence we have: a session that
 * just came up gets one full window to prove itself instead of being declared
 * stale immediately. */
static int64_t ws_app_reference_ms(void)
{
    int64_t app, connected;
    portENTER_CRITICAL(&s_ws_liveness_mux);
    app = s_ws_last_app_ms;
    connected = s_ws_connected_ms;
    portEXIT_CRITICAL(&s_ws_liveness_mux);
    return app > connected ? app : connected;
}

/* Application pings go out every s_ping_interval seconds (the endpoint's
 * ClientConfig PingInterval, 120 s by default) and the server answers each one,
 * so an idle but healthy session still shows an application frame every interval
 * plus latency.  Three intervals tolerate two lost pongs; the floor keeps a small
 * server-advertised interval from making the test jittery, and the ceiling bounds
 * it in case the endpoint ever advertises something absurd. */
#define FEISHU_WS_APP_STALE_MAX_MS (10 * 60 * 1000)

static int64_t ws_app_stale_limit_ms(void)
{
    int interval_s = s_ping_interval > 0 ? s_ping_interval : 120;
    int64_t limit = (int64_t)interval_s * 3 * 1000;
    if (limit < FEISHU_WS_RX_STALE_MS) limit = FEISHU_WS_RX_STALE_MS;
    if (limit > FEISHU_WS_APP_STALE_MAX_MS) limit = FEISHU_WS_APP_STALE_MAX_MS;
    return limit;
}

/* Count at most one reconnect per session: the four "session ended" events all
 * arrive for the same dead socket. */
static void ws_note_session_ended(uint32_t epoch)
{
    if (s_ws_counted_epoch == epoch) return;
    s_ws_counted_epoch = epoch;
    s_ws_reconnects++;
}

#define WS_EVENT_QUEUE_DEPTH 8
/* One outbound slot per queued event plus room for control frames; both queues
 * exist so that no caller ever blocks on the WebSocket task. */
#define WS_TX_QUEUE_DEPTH WS_EVENT_QUEUE_DEPTH
#define FEISHU_EVENT_TASK_STACK_BYTES 16384
#define FEISHU_EVENT_STACK_WARN_BYTES 1536
/* The event id has to survive the queue so the dedup entry can be written after
 * the event is handled; 80 matches feishu_event_dedup_t. */
#define WS_EVENT_ID_MAX 80
typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t config_generation;
    bool has_event_id;
    char event_id[WS_EVENT_ID_MAX];
} ws_job_t;
typedef struct { uint8_t *data; int len; } ws_tx_t;
static QueueHandle_t s_event_q;
static QueueHandle_t s_tx_q;
static TaskHandle_t s_event_task, s_tx_task;
static feishu_ws_assembler_t s_ws_assembler;
static feishu_pbbp_assembler_t s_pbbp_assembler;
static EXT_RAM_BSS_ATTR feishu_event_dedup_t s_event_dedup;

static void learn_peer_for_generation(const char *open_id, const char *chat_id,
                                     uint32_t generation);

/* ---------------- HTTP ---------------- */

static esp_err_t http_json(const char *url, const char *auth, const char *body,
                           char **out, int *status_out)
{
    if (!url || !body || strlen(body) > INT_MAX) return ESP_ERR_INVALID_ARG;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(c, "Content-Type", "application/json; charset=utf-8");
    if (auth && auth[0]) {
        char hdr[320];
        snprintf(hdr, sizeof(hdr), "Bearer %s", auth);
        esp_http_client_set_header(c, "Authorization", hdr);
    }
    esp_http_client_set_post_field(c, body, (int)strlen(body));

    esp_err_t err = esp_http_client_open(c, (int)strlen(body));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http open failed: %s (%s)", esp_err_to_name(err), url);
        esp_http_client_cleanup(c);
        return err;
    }
    size_t body_remaining = strlen(body);
    const char *body_cursor = body;
    while (body_remaining) {
        int chunk = body_remaining > 1024 ? 1024 : (int)body_remaining;
        int written = esp_http_client_write(c, body_cursor, chunk);
        if (written <= 0 || written > chunk) {
            ESP_LOGW(TAG, "http write failed (%s)", url);
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return ESP_FAIL;
        }
        body_cursor += written;
        body_remaining -= (size_t)written;
    }
    int hdr_len = esp_http_client_fetch_headers(c);
    if (hdr_len < 0) {
        ESP_LOGW(TAG, "http headers failed (%s)", url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(c);
    if (status_out) *status_out = status;
    if (status < 200 || status >= 300) ESP_LOGW(TAG, "http status %d (%s)", status, url);

    char *buf = calloc(1, 8192);
    int total = 0;
    if (buf) {
        while (total < 8191) {
            int n = esp_http_client_read(c, buf + total, 8191 - total);
            if (n <= 0) break;
            total += n;
        }
        buf[total] = 0;
    }
    (void)hdr_len;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (out) *out = buf;
    else free(buf);
    return ESP_OK;
}

/* ---------------- token ---------------- */

static esp_err_t token_refresh_locked(const char *app_id, const char *app_secret)
{
    if (!app_id || !app_id[0] || !app_secret || !app_secret[0]) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "app_id", app_id);
    cJSON_AddStringToObject(root, "app_secret", app_secret);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    char url[160];
    snprintf(url, sizeof(url), API_BASE TOKEN_PATH);

    char *resp = NULL;
    int status = 0;
    esp_err_t err = http_json(url, NULL, body, &resp, &status);
    free(body);
    if (err != ESP_OK || !resp) {
        ESP_LOGW(TAG, "token request failed: %s http=%d", esp_err_to_name(err), status);
        free(resp);
        s_token[0] = '\0';
        s_token_expire_ms = 0;
        return ESP_FAIL;
    }

    cJSON *j = cJSON_Parse(resp);
    if (!j) {
        ESP_LOGE(TAG, "token json parse failed");
        free(resp);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *code = cJSON_GetObjectItem(j, "code");
    cJSON *tok = cJSON_GetObjectItem(j, "tenant_access_token");
    cJSON *exp = cJSON_GetObjectItem(j, "expire");
    cJSON *msg = cJSON_GetObjectItem(j, "msg");
    long code_val = (code && cJSON_IsNumber(code)) ? (long)code->valueint : -1;
    if (code_val == 0 && tok && cJSON_IsString(tok) && tok->valuestring[0]) {
        strlcpy(s_token, tok->valuestring, sizeof(s_token));
        int expire = (exp && cJSON_IsNumber(exp) && exp->valueint > 60) ? exp->valueint : 7200;
        s_token_expire_ms = esp_timer_get_time() / 1000 + (int64_t)(expire - 60) * 1000;
        ret = ESP_OK;
        ESP_LOGI(TAG, "token ok, expire in %d s", expire);
    } else {
        /* Never keep a token the server refused: the next attempt must refresh. */
        s_token[0] = '\0';
        s_token_expire_ms = 0;
        ESP_LOGE(TAG, "token rejected: http=%d code=%ld msg=%s", status, code_val,
                 (msg && cJSON_IsString(msg)) ? msg->valuestring : "?");
    }
    cJSON_Delete(j);
    free(resp);
    return ret;
}

/* Copy the current tenant token into out, refreshing it when it is missing,
 * expired, or when force is set.  Callers must NOT hold s_token_lock. */
static bool token_get(char *out, size_t out_len, bool force, const char *app_id,
                      const char *app_secret)
{
    if (!out || out_len < 2) return false;
    if (!s_token_lock) s_token_lock = xSemaphoreCreateMutex();
    if (!s_token_lock) return false;
    xSemaphoreTake(s_token_lock, portMAX_DELAY);
    /* Snapshot first: feishu_init() bumps the epoch without taking this lock, so a
     * configuration change that lands while the refresh below is in flight must be
     * caught by re-reading it afterwards. */
    uint32_t epoch = s_token_epoch;
    bool ok = false;
    if (!force && s_token[0] && s_token_fill_epoch == epoch &&
        esp_timer_get_time() / 1000 < s_token_expire_ms) {
        ok = true;
    } else if (token_refresh_locked(app_id, app_secret) == ESP_OK) {
        if (s_token_epoch == epoch) {
            s_token_fill_epoch = epoch;
            ok = true;
        } else {
            /* The credentials changed while the request was in flight: the token we
             * just obtained belongs to the old ones, so leave the cache empty. */
            s_token[0] = '\0';
            s_token_expire_ms = 0;
        }
    }
    if (ok) strlcpy(out, s_token, out_len);
    xSemaphoreGive(s_token_lock);
    return ok;
}

/* ---------------- 发送 ---------------- */

static esp_err_t send_common(const char *url, const char *rid, const char *text, const char *uuid,
                             uint32_t expected_generation,
                             char *msg_id, size_t msg_id_len, bool *retryable_out)
{
    if (retryable_out) *retryable_out=false;
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    /* Snapshot the credentials and release the config lock before touching the
     * network: holding it across a 10 s HTTP request stalled every other caller
     * (including the long-connection setup) and made the status page report a
     * healthy connection as offline for the whole retry cycle. */
    char app_id[sizeof(s_cfg.app_id)], app_secret[sizeof(s_cfg.app_secret)];
    uint32_t generation;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(app_id, s_cfg.app_id, sizeof(app_id));
    strlcpy(app_secret, s_cfg.app_secret, sizeof(app_secret));
    generation = s_cfg_generation;
    xSemaphoreGive(s_lock);
    if (expected_generation != UINT32_MAX && expected_generation != generation)
        return ESP_ERR_INVALID_STATE;
    if (!app_id[0] || !app_secret[0]) return ESP_ERR_INVALID_STATE;

    cJSON *inner = cJSON_CreateObject();
    if (!inner) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(inner, "text", text);
    char *inner_str = cJSON_PrintUnformatted(inner);
    cJSON_Delete(inner);
    if (!inner_str) return ESP_ERR_NO_MEM;

    cJSON *root = cJSON_CreateObject();
    if (!root) { free(inner_str); return ESP_ERR_NO_MEM; }
    if (rid && rid[0]) cJSON_AddStringToObject(root, "receive_id", rid);
    cJSON_AddStringToObject(root, "msg_type", "text");
    cJSON_AddStringToObject(root, "content", inner_str);
    if (uuid && uuid[0]) cJSON_AddStringToObject(root, "uuid", uuid);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(inner_str);
    if (!body) return ESP_ERR_NO_MEM;

    char tok[256];
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!token_get(tok, sizeof(tok), attempt > 0, app_id, app_secret)) {
            if (retryable_out) *retryable_out = true;
            ret = ESP_FAIL;
            break;
        }
        char *resp = NULL;
        int status = 0;
        esp_err_t err = http_json(url, tok, body, &resp, &status);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send request failed: %s http=%d", esp_err_to_name(err), status);
            if (retryable_out && feishu_retry_policy(err != ESP_ERR_NO_MEM, status))
                *retryable_out = true;
            free(resp);
            ret = err;
            break;
        }
        cJSON *j = resp ? cJSON_Parse(resp) : NULL;
        cJSON *code = j ? cJSON_GetObjectItem(j, "code") : NULL;
        long code_val = (code && cJSON_IsNumber(code)) ? (long)code->valueint : -1;
        if (status >= 200 && status < 300 && code_val == 0) {
            cJSON *data = j ? cJSON_GetObjectItem(j, "data") : NULL;
            cJSON *mid = data ? cJSON_GetObjectItem(data, "message_id") : NULL;
            ret = ESP_FAIL;
            if (msg_id && msg_id_len) {
                if (mid && cJSON_IsString(mid) && mid->valuestring[0] &&
                    strlen(mid->valuestring) < msg_id_len) {
                    strlcpy(msg_id, mid->valuestring, msg_id_len);
                    ret = ESP_OK;
                }
            } else {
                ret = ESP_OK;
            }
            cJSON_Delete(j);
            free(resp);
            break;
        }
        /* The API reports most logical failures as HTTP 200 with a non-zero code. */
        cJSON *msg = j ? cJSON_GetObjectItem(j, "msg") : NULL;
        bool stale_token = feishu_should_retry_after_reject(attempt > 0, code_val, status);
        const char *msg_str = (msg && cJSON_IsString(msg)) ? msg->valuestring : "?";
        if (stale_token) {
            ESP_LOGW(TAG, "send rejected (http=%d code=%ld msg=%s), refreshing token",
                     status, code_val, msg_str);
        } else {
            ESP_LOGE(TAG, "send rejected (http=%d code=%ld msg=%s)", status, code_val, msg_str);
        }
        cJSON_Delete(j);
        free(resp);
        if (status >= 500) {
            if (retryable_out) *retryable_out = true;
            ret = ESP_FAIL;
            break;
        }
        if (stale_token) continue;
        ret = ESP_FAIL;
        break;
    }
    if (ret == ESP_OK) {
        __atomic_store_n(&s_send_fail_streak, 0, __ATOMIC_RELAXED);
    } else if (__atomic_load_n(&s_send_fail_streak, __ATOMIC_RELAXED) < 1000) {
        /* Saturate: the exact number is irrelevant once the channel counts as down. */
        (void)__atomic_add_fetch(&s_send_fail_streak, 1, __ATOMIC_RELAXED);
    }
    free(body);
    return ret;
}

uint32_t feishu_send_fail_streak(void)
{
    return __atomic_load_n(&s_send_fail_streak, __ATOMIC_RELAXED);
}

esp_err_t feishu_send_text(const char *text, char *msg_id, size_t msg_id_len)
{
    return feishu_send_text_uuid(text,NULL,msg_id,msg_id_len);
}

esp_err_t feishu_send_text_uuid(const char *text, const char *uuid,
                                char *msg_id, size_t msg_id_len)
{
    char url[512];
    char rid[64];
    uint32_t generation;
    if (s_lock) xSemaphoreTake(s_lock,portMAX_DELAY);
    strlcpy(rid,(s_cfg.chat_id[0] && s_cfg.open_id[0] == '\0') ? s_cfg.chat_id : s_cfg.open_id,sizeof(rid));
    generation=s_cfg_generation;
    if (s_lock) xSemaphoreGive(s_lock);
    if (!rid[0]) return ESP_ERR_INVALID_STATE;
    const char *type = (strncmp(rid, "oc_", 3) == 0) ? "chat_id" : "open_id";
    snprintf(url, sizeof(url), API_BASE MSG_PATH "?receive_id_type=%s&receive_id=%s",
             type, rid);
    return send_common(url, rid, text, uuid, generation, msg_id, msg_id_len, NULL);
}

esp_err_t feishu_reply_text(const char *parent_id, const char *text,
                            char *msg_id, size_t msg_id_len)
{
    if (!parent_id || !parent_id[0]) return ESP_ERR_INVALID_ARG;
    char uuid[51];
    if(!feishu_reply_uuid(parent_id,uuid)) return ESP_ERR_INVALID_ARG;
    char url[512];
    snprintf(url, sizeof(url), API_BASE MSG_PATH "/%s/reply", parent_id);
    uint32_t generation;
    if(s_lock) xSemaphoreTake(s_lock,portMAX_DELAY);
    generation=s_cfg_generation;
    if(s_lock) xSemaphoreGive(s_lock);
    char reply_id[96];
    char *result_id=(msg_id && msg_id_len)?msg_id:reply_id;
    size_t result_id_len=(msg_id && msg_id_len)?msg_id_len:sizeof(reply_id);
    for(int attempt=0;attempt<2;attempt++) {
        bool retryable=false;
        esp_err_t err=send_common(url,NULL,text,uuid,generation,result_id,result_id_len,&retryable);
        if(err==ESP_OK || !retryable || attempt==1) return err;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return ESP_FAIL;
}

/* ---------------- MMS media replies ---------------- */

#define MEDIA_HTTP_TIMEOUT_MS 15000
#define MEDIA_HTTP_RESPONSE_MAX 4096
#define MEDIA_MULTIPART_BOUNDARY "----smsf-mms-7MA4YWxkTrZu0gW"

static bool media_safe_id(const char *id, const char *prefix, size_t max_len)
{
    if (!id || !id[0] || strlen(id) > max_len || (prefix && strncmp(id, prefix, strlen(prefix)) != 0)) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}

static bool media_write_all(esp_http_client_handle_t client, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        int chunk = len > 1024 ? 1024 : (int)len;
        int n = esp_http_client_write(client, p, chunk);
        if (n <= 0 || n > chunk) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool media_json_key(char *json, int status, const char *key, char *out, size_t out_len)
{
    if (!json || status < 200 || status >= 300 || !key || !out || !out_len) return false;
    cJSON *root = cJSON_Parse(json);
    cJSON *code = root ? cJSON_GetObjectItem(root, "code") : NULL;
    cJSON *data = root ? cJSON_GetObjectItem(root, "data") : NULL;
    cJSON *item = data ? cJSON_GetObjectItem(data, key) : NULL;
    bool ok = code && cJSON_IsNumber(code) && code->valueint == 0 &&
              item && cJSON_IsString(item) && item->valuestring[0] &&
              strlen(item->valuestring) < out_len;
    if (ok) strlcpy(out, item->valuestring, out_len);
    if (root) cJSON_Delete(root);
    return ok;
}

static esp_err_t media_upload(const char *url, const char *token,
                              const feishu_media_part_t *part,
                              feishu_media_kind_t kind, const char *safe_name,
                              const char *mime, char *key, size_t key_len)
{
    if (!url || !token || !token[0] || !part || !part->data || !part->size ||
        !safe_name || !mime || !key || !key_len) return ESP_ERR_INVALID_ARG;

    char fields[320], file_header[320], tail[64];
    int fields_len;
    if (kind == FEISHU_MEDIA_IMAGE) {
        fields_len = snprintf(fields, sizeof(fields),
            "--%s\r\nContent-Disposition: form-data; name=\"image_type\"\r\n\r\nmessage\r\n--%s\r\n",
            MEDIA_MULTIPART_BOUNDARY, MEDIA_MULTIPART_BOUNDARY);
    } else if (kind == FEISHU_MEDIA_PDF) {
        fields_len = snprintf(fields, sizeof(fields),
            "--%s\r\nContent-Disposition: form-data; name=\"file_type\"\r\n\r\npdf\r\n"
            "--%s\r\nContent-Disposition: form-data; name=\"file_name\"\r\n\r\n%s\r\n--%s\r\n",
            MEDIA_MULTIPART_BOUNDARY, MEDIA_MULTIPART_BOUNDARY, safe_name, MEDIA_MULTIPART_BOUNDARY);
    } else return ESP_ERR_INVALID_ARG;
    if (fields_len < 0 || (size_t)fields_len >= sizeof(fields)) return ESP_ERR_INVALID_SIZE;
    const char *field_name = kind == FEISHU_MEDIA_IMAGE ? "image" : "file";
    int file_header_len = snprintf(file_header, sizeof(file_header),
        "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n", field_name, safe_name, mime);
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", MEDIA_MULTIPART_BOUNDARY);
    if (file_header_len < 0 || (size_t)file_header_len >= sizeof(file_header) ||
        tail_len < 0 || (size_t)tail_len >= sizeof(tail)) return ESP_ERR_INVALID_SIZE;
    size_t content_len = (size_t)fields_len + (size_t)file_header_len + part->size + (size_t)tail_len;
    if (content_len > (size_t)INT_MAX) return ESP_ERR_INVALID_SIZE;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MEDIA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    char auth[320];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=" MEDIA_MULTIPART_BOUNDARY);

    esp_err_t err = esp_http_client_open(client, (int)content_len);
    bool wrote = err == ESP_OK && media_write_all(client, fields, (size_t)fields_len) &&
                 media_write_all(client, file_header, (size_t)file_header_len) &&
                 media_write_all(client, part->data, part->size) &&
                 media_write_all(client, tail, (size_t)tail_len);
    char *response = NULL;
    int status = 0;
    if (wrote) {
        int hdr = esp_http_client_fetch_headers(client);
        if (hdr >= 0) {
            status = esp_http_client_get_status_code(client);
            response = calloc(1, MEDIA_HTTP_RESPONSE_MAX);
            if (response) {
                int total = 0;
                while (total < MEDIA_HTTP_RESPONSE_MAX - 1) {
                    int n = esp_http_client_read(client, response + total, MEDIA_HTTP_RESPONSE_MAX - 1 - total);
                    if (n <= 0) break;
                    total += n;
                }
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!wrote) { free(response); return err == ESP_OK ? ESP_FAIL : err; }
    if (!response) return ESP_ERR_NO_MEM;
    bool ok = media_json_key(response, status, kind == FEISHU_MEDIA_IMAGE ? "image_key" : "file_key", key, key_len);
    free(response);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t media_send_reply(const char *parent_id, const char *token,
                                  const char *msg_type, const char *content_json,
                                  const char *uuid, char *message_id, size_t message_id_len)
{
    char url[256];
    int url_len = snprintf(url, sizeof(url), API_BASE MSG_PATH "/%s/reply", parent_id);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) return ESP_ERR_INVALID_SIZE;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "msg_type", msg_type);
    cJSON_AddStringToObject(root, "content", content_json);
    cJSON_AddStringToObject(root, "uuid", uuid);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    char *response = NULL;
    int status = 0;
    esp_err_t err = http_json(url, token, body, &response, &status);
    free(body);
    if (err != ESP_OK) { free(response); return err; }
    bool ok = media_json_key(response, status, "message_id", message_id, message_id_len);
    free(response);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t media_upload_part(const char *token, const feishu_media_part_t *part,
                                   feishu_media_kind_t kind, char *key, size_t key_len)
{
    char name[64];
    const char *extension = kind == FEISHU_MEDIA_IMAGE ? feishu_media_image_extension(part->mime_type) : "pdf";
    if (!feishu_media_trusted_filename(part->filename, extension, name, sizeof(name))) return ESP_ERR_INVALID_ARG;
    const char *mime = feishu_media_canonical_mime(part->mime_type);
    if (!mime) return ESP_ERR_INVALID_ARG;
    return media_upload(kind == FEISHU_MEDIA_IMAGE ? API_BASE "/open-apis/im/v1/images" : API_BASE "/open-apis/im/v1/files",
                        token, part, kind, name, mime, key, key_len);
}

static bool media_generation_current(uint32_t generation)
{
    bool current;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    current = generation == s_cfg_generation;
    xSemaphoreGive(s_lock);
    return current;
}

static char *media_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

esp_err_t feishu_send_media_parts(const char *parent_id,
                                  const feishu_media_part_t *parts, size_t count,
                                  const char *stable_id_prefix,
                                  feishu_media_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));
    if (!parent_id || !media_safe_id(parent_id, "om_", 96) ||
        (!parts && count) || !stable_id_prefix ||
        !media_safe_id(stable_id_prefix, "mms_", 46) || !result) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (count > FEISHU_MEDIA_PARTS_MAX) result->skipped_count = count - FEISHU_MEDIA_PARTS_MAX;
    char token[sizeof(s_token)];
    char app_id[sizeof(s_cfg.app_id)], app_secret[sizeof(s_cfg.app_secret)];
    uint32_t generation;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    strlcpy(app_id, s_cfg.app_id, sizeof(app_id));
    strlcpy(app_secret, s_cfg.app_secret, sizeof(app_secret));
    generation = s_cfg_generation;
    xSemaphoreGive(s_lock);
    if (!token_get(token, sizeof(token), false, app_id, app_secret)) return ESP_FAIL;

    size_t bounded = count < FEISHU_MEDIA_PARTS_MAX ? count : FEISHU_MEDIA_PARTS_MAX;
    size_t total_bytes = 0;
    esp_err_t result_error = ESP_OK;
    for (size_t i = 0; i < bounded; ++i) {
        if (!media_generation_current(generation)) {
            result_error = ESP_ERR_INVALID_STATE;
            break;
        }
        const feishu_media_part_t *part = &parts[i];
        feishu_media_kind_t kind = feishu_media_classify(part->mime_type);
        if (kind == FEISHU_MEDIA_UNSUPPORTED || !part->data || part->size == 0 ||
            part->size > FEISHU_MEDIA_TOTAL_MAX_BYTES || part->size > FEISHU_MEDIA_TOTAL_MAX_BYTES - total_bytes) {
            result->skipped_count++;
            continue;
        }
        total_bytes += part->size;
        if (kind != FEISHU_MEDIA_TEXT &&
            !feishu_media_payload_matches(part->mime_type, part->data, part->size)) {
            result->skipped_count++;
            continue;
        }
        char uuid[51];
        int uuid_len = snprintf(uuid, sizeof(uuid), "%s_p%02u", stable_id_prefix, (unsigned)i);
        if (uuid_len < 0 || (size_t)uuid_len >= sizeof(uuid)) {
            result->skipped_count++;
            continue;
        }
        char message_id[64] = "";
        esp_err_t err;
        if (kind == FEISHU_MEDIA_TEXT) {
            if (part->size > FEISHU_MEDIA_TEXT_MAX_BYTES) {
                result->skipped_count++;
                continue;
            }
            char *decoded = media_alloc(FEISHU_MEDIA_TEXT_MAX_BYTES + 1);
            if (!decoded) { result_error = ESP_ERR_NO_MEM; break; }
            size_t decoded_len = 0;
            if (!feishu_media_decode_text(part->charset, part->data, part->size,
                                          decoded, FEISHU_MEDIA_TEXT_MAX_BYTES + 1,
                                          &decoded_len) || decoded_len > FEISHU_MEDIA_TEXT_MAX_BYTES) {
                free(decoded);
                result->skipped_count++;
                continue;
            }
            cJSON *content = cJSON_CreateObject();
            if (!content || !cJSON_AddStringToObject(content, "text", decoded)) {
                if (content) cJSON_Delete(content);
                free(decoded);
                result_error = ESP_ERR_NO_MEM;
                break;
            }
            char *content_json = cJSON_PrintUnformatted(content);
            cJSON_Delete(content);
            free(decoded);
            if (!content_json) { result_error = ESP_ERR_NO_MEM; break; }
            if (strlen(content_json) > FEISHU_MEDIA_TEXT_MAX_BYTES + 256) {
                free(content_json);
                result->skipped_count++;
                continue;
            }
            err = media_send_reply(parent_id, token, "text", content_json, uuid,
                                          message_id, sizeof(message_id));
            free(content_json);
        } else {
            char key[128] = "";
            err = media_upload_part(token, part, kind, key, sizeof(key));
            if (err == ESP_OK) {
                if (!media_generation_current(generation)) {
                    result_error = ESP_ERR_INVALID_STATE;
                    break;
                }
                cJSON *content = cJSON_CreateObject();
                const char *key_name = kind == FEISHU_MEDIA_IMAGE ? "image_key" : "file_key";
                if (!content || !cJSON_AddStringToObject(content, key_name, key)) {
                    if (content) cJSON_Delete(content);
                    result_error = ESP_ERR_NO_MEM;
                    break;
                }
                char *content_json = cJSON_PrintUnformatted(content);
                cJSON_Delete(content);
                if (!content_json) { result_error = ESP_ERR_NO_MEM; break; }
                err = media_send_reply(parent_id, token,
                    kind == FEISHU_MEDIA_IMAGE ? "image" : "file", content_json, uuid,
                    message_id, sizeof(message_id));
                free(content_json);
            }
        }
        if (err != ESP_OK) { result_error = err; break; }
        if (result->sent_count < FEISHU_MEDIA_PARTS_MAX) {
            result->part_indexes[result->sent_count] = i;
            strlcpy(result->message_ids[result->sent_count], message_id,
                    sizeof(result->message_ids[result->sent_count]));
            result->sent_count++;
        }
    }
    return result_error;
}

/* ---------------- 长连接 ---------------- */

/* pbbp2_encode() writes the payload plus its envelope: every one of
 * PBBP2_MAX_HEADERS header slots contributes a length varint and the key/value
 * bytes, budgeted at 128 B each for the short keys and values we set, and the
 * fixed fields plus the payload length varint fit in 1 KiB.  With the current 24
 * header slots that is the historical 4 KiB scratch size. */
#define WS_TX_HEADER_BUDGET_BYTES 128
#define WS_TX_FRAME_OVERHEAD_BYTES (PBBP2_MAX_HEADERS * WS_TX_HEADER_BUDGET_BYTES + 1024)

static bool ws_send_frame(pbbp2_frame_t *f)
{
    if (!s_ws || !s_ws_connected || !s_tx_q || !f || f->payload_len > FEISHU_WS_MESSAGE_MAX) return false;
    uint8_t *buf = malloc(WS_TX_FRAME_OVERHEAD_BYTES + f->payload_len);
    if (!buf) return false;
    int n = pbbp2_encode(f, buf, WS_TX_FRAME_OVERHEAD_BYTES + f->payload_len);
    if (n > 0) {
        ws_tx_t tx={.data=buf,.len=n};
        if (xQueueSend(s_tx_q,&tx,0)==pdTRUE) return true;
    }
    free(buf);
    return false;
}

static void ws_tx_task(void *arg)
{
    (void)arg;
    ws_tx_t tx;
    while (xQueueReceive(s_tx_q,&tx,portMAX_DELAY)==pdTRUE) {
        if (s_ws_lock) xSemaphoreTake(s_ws_lock,portMAX_DELAY);
        if (s_ws && s_ws_connected) esp_websocket_client_send_bin(s_ws,(const char *)tx.data,tx.len,pdMS_TO_TICKS(1000));
        if (s_ws_lock) xSemaphoreGive(s_ws_lock);
        free(tx.data);
    }
}

static bool ws_ack(pbbp2_frame_t *f, int code, int elapsed_ms)
{
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "%d", elapsed_ms);
    /* Without biz_rt the gateway cannot measure the round trip, but the ack is
     * still worth sending, so this is logged rather than aborting. */
    if (pbbp2_header_set(f, "biz_rt", hdr) != 0) ESP_LOGW(TAG, "ack header set failed");
    char body[32];
    snprintf(body, sizeof(body), "{\"code\":%d}", code);
    free(f->payload);
    f->payload = (uint8_t *)strdup(body);
    if (!f->payload) { f->payload_len=0; return false; }
    f->payload_len = strlen(body);
    return ws_send_frame(f);
}

static void handle_event_payload(const uint8_t *payload, size_t payload_len, uint32_t generation)
{
    char *json = strndup((const char *)payload, payload_len);
    cJSON *j = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!j) {
        ESP_LOGW(TAG, "event JSON parse/allocation failed");
        return;
    }

    cJSON *header = cJSON_GetObjectItem(j, "header");
    cJSON *etype = header ? cJSON_GetObjectItem(header, "event_type") : NULL;
    ESP_LOGD(TAG, "event type=%s", (etype && cJSON_IsString(etype)) ? etype->valuestring : "?");
    if (!etype || !cJSON_IsString(etype) ||
        strcmp(etype->valuestring, "im.message.receive_v1") != 0) {
        cJSON_Delete(j);
        return;
    }

    cJSON *event = cJSON_GetObjectItem(j, "event");
    cJSON *msg = event ? cJSON_GetObjectItem(event, "message") : NULL;
    cJSON *sender = event ? cJSON_GetObjectItem(event, "sender") : NULL;
    cJSON *sender_id = sender ? cJSON_GetObjectItem(sender, "sender_id") : NULL;

    const char *mid = NULL, *parent = NULL, *chat = NULL, *mtype = NULL, *content = NULL;
    const char *open_id = NULL;
    const char *sender_type = NULL;
    cJSON *v;
    if (msg) {
        v = cJSON_GetObjectItem(msg, "message_id");  if (v && cJSON_IsString(v)) mid = v->valuestring;
        v = cJSON_GetObjectItem(msg, "parent_id");   if (v && cJSON_IsString(v)) parent = v->valuestring;
        if (!parent || !parent[0]) { v = cJSON_GetObjectItem(msg, "root_id"); if (v && cJSON_IsString(v)) parent = v->valuestring; }
        v = cJSON_GetObjectItem(msg, "chat_id");     if (v && cJSON_IsString(v)) chat = v->valuestring;
        v = cJSON_GetObjectItem(msg, "message_type");if (v && cJSON_IsString(v)) mtype = v->valuestring;
        v = cJSON_GetObjectItem(msg, "content");     if (v && cJSON_IsString(v)) content = v->valuestring;
    }
    if (sender_id) {
        v = cJSON_GetObjectItem(sender_id, "open_id");
        if (v && cJSON_IsString(v)) open_id = v->valuestring;
    }
    v = sender ? cJSON_GetObjectItem(sender, "sender_type") : NULL;
    if (v && cJSON_IsString(v)) sender_type = v->valuestring;

    /* Only accept direct bot conversations; never learn a group or arbitrary sender. */
    cJSON *chat_type = msg ? cJSON_GetObjectItem(msg, "chat_type") : NULL;
    bool p2p = chat_type && cJSON_IsString(chat_type) && strcmp(chat_type->valuestring, "p2p") == 0 &&
               sender_type && strcmp(sender_type, "user") == 0;
    char configured_open[sizeof(s_cfg.open_id)], configured_chat[sizeof(s_cfg.chat_id)];
    if(s_lock) xSemaphoreTake(s_lock,portMAX_DELAY);
    strlcpy(configured_open,s_cfg.open_id,sizeof(configured_open));
    strlcpy(configured_chat,s_cfg.chat_id,sizeof(configured_chat));
    if(s_lock) xSemaphoreGive(s_lock);
    bool own_peer;
    if (configured_open[0]) own_peer = open_id && strcmp(open_id,configured_open)==0;
    else if (configured_chat[0]) own_peer = chat && strcmp(chat,configured_chat)==0;
    else own_peer = true;
    if (p2p && own_peer && open_id && chat) {
        learn_peer_for_generation(open_id,chat,generation);
    }

    if (mtype && strcmp(mtype, "text") == 0 && content) {
        cJSON *cj = cJSON_Parse(content);
        cJSON *t = cj ? cJSON_GetObjectItem(cj, "text") : NULL;
        if (t && cJSON_IsString(t) && s_cmd_cb && p2p && own_peer && open_id && chat &&
            generation == s_cfg_generation) {
            s_cmd_cb(open_id ? open_id : "", chat ? chat : "", t->valuestring,
                     mid ? mid : "", parent ? parent : "");
        }
        if (!cj) ESP_LOGW(TAG, "message content parse failed");
        if (cj) cJSON_Delete(cj);
    }
    cJSON_Delete(j);
}

static void event_worker(void *arg)
{
    (void)arg;
    ws_job_t job;
    while (xQueueReceive(s_event_q, &job, portMAX_DELAY) == pdTRUE) {
        if (job.config_generation != s_cfg_generation) {
            /* The ack already went out when this job was queued, so nothing will
             * ever redeliver the event; say which generation change ate it. */
            ESP_LOGW(TAG, "dropping queued event: config generation %lu -> %lu",
                     (unsigned long)job.config_generation, (unsigned long)s_cfg_generation);
        } else if (job.has_event_id &&
                   feishu_event_dedup_contains(&s_event_dedup, job.event_id)) {
            /* The id is only remembered after handling, so two redeliveries of the
             * same event can now be queued at once; the worker is the one place
             * that closes that window. */
            ESP_LOGW(TAG, "dropping duplicate queued event");
        } else {
            handle_event_payload(job.data, job.len, job.config_generation);
            /* Remembering the id here instead of at enqueue time is what makes a
             * generation bump (or a crash) mid-queue replayable: the command was
             * not executed, so it must not be filtered as seen. */
            if (job.has_event_id) feishu_event_dedup_remember(&s_event_dedup, job.event_id);
        }
        free(job.data);
        /* ESP-IDF reports uxTaskGetStackHighWaterMark in bytes. */
        UBaseType_t low_water_bytes = uxTaskGetStackHighWaterMark(NULL);
        if (low_water_bytes < FEISHU_EVENT_STACK_WARN_BYTES) {
            ESP_LOGW(TAG, "fs_event stack low-water: %u bytes", (unsigned)low_water_bytes);
        } else {
            ESP_LOGD(TAG, "fs_event stack low-water: %u bytes", (unsigned)low_water_bytes);
        }
    }
}

/* The server's answer to the application ping we send every s_ping_interval
 * seconds.  The type arrives as the PBBP2 "type" header; payload_type is checked
 * too because a gateway change could move it to the protobuf field of that name.
 * Both spellings mean the same thing to us: the Feishu application layer is
 * talking. */
static bool ws_frame_is_app_pong(const pbbp2_frame_t *f)
{
    if (f->method != 0) return false;
    const char *type = pbbp2_header_get(f, "type");
    if (type && strcmp(type, "pong") == 0) return true;
    return f->payload_type[0] && strcmp(f->payload_type, "pong") == 0;
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint32_t handler_epoch=(uint32_t)(uintptr_t)arg;
    if(handler_epoch!=s_ws_epoch || s_ws_cfg_generation!=s_cfg_generation) return;
    uint32_t handler_generation=s_ws_cfg_generation;
    int64_t callback_start_ms = esp_timer_get_time() / 1000;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        /* Both stamps belong to this session: the transport clock starts now and
         * the application clock waits for the first application-level frame. */
        ws_liveness_note_connected(callback_start_ms);
        s_ws_online_epoch = handler_epoch;
        s_ws_connected = true;
        s_ws_forced_dead = false;
        /* A frame that was cut in half by the previous session can never be
         * completed by this one; dropping the partial buffers here is what makes
         * the reassemblers safe across reconnects (they otherwise self-reset only
         * on the next offset mismatch, keeping a malloc'd buffer alive). */
        feishu_ws_assembler_reset(&s_ws_assembler);
        feishu_pbbp_assembler_reset(&s_pbbp_assembler);
        ESP_LOGI(TAG, "ws connected, driver ping interval %u s",
                 (unsigned)esp_websocket_client_get_ping_interval_sec(s_ws));
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_ws_connected = false;
        ws_note_session_ended(handler_epoch);
        ESP_LOGW(TAG, "ws disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (d) {
            ESP_LOGW(TAG, "ws error: type=%d tls=%d stack=%d sock_errno=%d handshake=%d",
                     (int)d->error_handle.error_type, (int)d->error_handle.esp_tls_last_esp_err,
                     (int)d->error_handle.esp_tls_stack_err,
                     (int)d->error_handle.esp_transport_sock_errno,
                     (int)d->error_handle.esp_ws_handshake_status_code);
        }
        /* fall through */
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH:
        /* A clean server-side CLOSE makes the library stop the client task
         * without ever emitting DISCONNECTED, and abort_connection() skips the
         * CLOSING state entirely.  Leaving s_ws_connected true here was the
         * permanent outage: feishu_ws_start() then returned early forever and
         * net_task never reconnected. */
        s_ws_connected = false;
        /* ERROR, DISCONNECTED, CLOSED and FINISH all describe this one dead
         * session: the caller counts sessions, not events. */
        ws_note_session_ended(handler_epoch);
        ESP_LOGW(TAG, "ws session ended (event %d)", (int)id);
        break;
    case WEBSOCKET_EVENT_DATA:
        /* Every frame refreshes the transport clock - including the driver's own
         * RFC6455 PING/PONG exchange (op_code 0x0A, one every 10 s), which arrives
         * here as a DATA event too.  That is exactly why it must not refresh the
         * application clock: a wedged Feishu session keeps answering transport
         * pings and would look healthy forever. */
        ws_liveness_note_ctrl(callback_start_ms);
        ESP_LOGD(TAG, "ws data op=%d off=%d len=%d total=%d", d->op_code, d->payload_offset,
                 d->data_len, d->payload_len);
        if (d->op_code == 0x00 || d->op_code == 0x02 || d->op_code == 0x01) {
            uint8_t *wire=NULL; size_t wire_len=0;
            /* The driver posts one DATA event per read chunk (at most .buffer_size,
             * 8192 B) and only sets fin on the last chunk of the frame, so fin has
             * to be derived from the offset or every multi-chunk frame is dropped
             * unacked. */
            bool frame_fin = (long)d->payload_offset + (long)d->data_len >= (long)d->payload_len;
            int assembled=feishu_ws_assembler_feed(&s_ws_assembler,(size_t)d->payload_offset,
                    (size_t)d->payload_len,d->data_ptr,(size_t)d->data_len,frame_fin,&wire,&wire_len);
            if (assembled==1) {
                pbbp2_frame_t f;
                if (pbbp2_decode(wire,wire_len,&f)==0) {
                    if (f.method==0) {
                        if (ws_frame_is_app_pong(&f)) {
                            ws_liveness_note_app(callback_start_ms);
                            if (f.payload) {
                                cJSON *cj=cJSON_Parse((const char *)f.payload);
                                if(cj) {
                                    cJSON *pi=cJSON_GetObjectItem(cj,"PingInterval");
                                    if(pi && pi->valueint>0) s_ping_interval=pi->valueint;
                                    cJSON_Delete(cj);
                                }
                            }
                        }
                    } else {
                        const char *type=pbbp2_header_get(&f,"type");
                        if(type && strcmp(type,"event")==0 && f.payload) {
                            const char *mid=pbbp2_header_get(&f,"message_id");
                            const char *sum_h=pbbp2_header_get(&f,"sum");
                            const char *seq_h=pbbp2_header_get(&f,"seq");
                            int sum=sum_h?atoi(sum_h):1, seq=seq_h?atoi(seq_h):0;
                            uint8_t *payload=NULL; size_t payload_len=0;
                            int ready;
                            if (sum <= 1) {
                                /* Single-frame events: the protocol only requires a
                                 * message_id when sum > 1, so handing these to the
                                 * multi-part assembler dropped them unacked. */
                                payload = malloc(f.payload_len ? f.payload_len : 1);
                                if (payload) { memcpy(payload, f.payload, f.payload_len); payload_len = f.payload_len; ready = 1; }
                                else ready = -1;
                            } else {
                                ready=feishu_pbbp_assembler_feed(&s_pbbp_assembler,mid,sum,seq,
                                    f.payload,f.payload_len,esp_timer_get_time()/1000,&payload,&payload_len);
                            }
                            if(ready==1) {
                                /* The application layer delivered a complete event:
                                 * this is the signal feishu_ws_alive() trusts, and
                                 * it is independent of how the event is routed
                                 * below (a queue-full drop still proves delivery). */
                                ws_liveness_note_app(callback_start_ms);
                                cJSON *ej=cJSON_ParseWithLength((const char *)payload,payload_len);
                                cJSON *eh=ej?cJSON_GetObjectItem(ej,"header"):NULL;
                                cJSON *eid=eh?cJSON_GetObjectItem(eh,"event_id"):NULL;
                                bool has_id=eid&&cJSON_IsString(eid)&&eid->valuestring[0];
                                bool duplicate=has_id&&feishu_event_dedup_contains(&s_event_dedup,eid->valuestring);
                                ws_job_t job={.data=payload,.len=payload_len,
                                              .config_generation=handler_generation,
                                              .has_event_id=has_id};
                                if(has_id) strlcpy(job.event_id,eid->valuestring,sizeof(job.event_id));
                                if(duplicate) {
                                    free(payload);
                                    ESP_LOGW(TAG, "duplicate event id, acking without running it");
                                    (void)ws_ack(&f,200,(int)(esp_timer_get_time()/1000-callback_start_ms));
                                } else if(s_event_q && xQueueSend(s_event_q,&job,0)==pdTRUE) {
                                    /* The dedup entry is written by event_worker(),
                                     * after the event was actually handled: an id
                                     * remembered here survived a configuration
                                     * change that made the worker drop the command. */
                                    (void)ws_ack(&f,200,(int)(esp_timer_get_time()/1000-callback_start_ms));
                                } else {
                                    ESP_LOGW(TAG, "event queue full, dropping event unacked");
                                    free(payload);
                                }
                                if(ej) cJSON_Delete(ej);
                            }
                        }
                    }
                    pbbp2_frame_free(&f);
                } else {
                    ESP_LOGW(TAG, "pbbp2 decode failed (%u B) on op_code %d",
                             (unsigned)wire_len, (int)d->op_code);
                }
                free(wire);
            }
        }
        break;
    default:
        break;
    }
}

/* Sends the application-level keepalive.  Liveness is *not* judged here: the
 * supervisor (net_health.c) owns the staleness threshold and the repair ladder, so
 * there is exactly one place that decides a session is dead. */
static void ping_task(void *arg)
{
    (void)arg;
    while (1) {
        int interval = s_ping_interval > 0 ? s_ping_interval : 120;
        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
        if (!s_ws_connected) continue;
        pbbp2_frame_t f;
        pbbp2_frame_init(&f);
        f.method = 0;
        f.service = s_service_id;
        pbbp2_header_set(&f, "type", "ping");
        (void)ws_send_frame(&f);
        pbbp2_frame_free(&f);
        ESP_LOGD(TAG, "ping sent");
    }
}

esp_err_t feishu_ws_start(void)
{
    if (!s_lock) s_lock=xSemaphoreCreateMutex();
    if (!s_ws_lock) s_ws_lock=xSemaphoreCreateMutex();
    if (!s_ws_lifecycle_lock) s_ws_lifecycle_lock=xSemaphoreCreateMutex();
    if (!s_token_lock) s_token_lock=xSemaphoreCreateMutex();
    if (!s_lock || !s_ws_lock || !s_ws_lifecycle_lock || !s_token_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_ws_lifecycle_lock,portMAX_DELAY);
    feishu_cfg_t cfg;
    uint32_t config_generation;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    cfg=s_cfg;
    config_generation=s_cfg_generation;
    bool configured=cfg.app_id[0] && cfg.app_secret[0];
    xSemaphoreGive(s_lock);
    if (!configured) { xSemaphoreGive(s_ws_lifecycle_lock); return ESP_ERR_INVALID_STATE; }
    int64_t now_ms=esp_timer_get_time()/1000;

    /* Reuse a session that is either live or younger than WS_RECREATE_MIN_AGE_MS
     * - a younger one may still be handshaking, so "not connected yet" is not
     * evidence of anything.  The connected flag alone is not trusted either: a
     * socket the gateway closed cleanly has already lost its task while our flag
     * may still say connected, so ask the driver as well. */
    bool session_present, session_usable = false, session_alive = false;
    bool session_fresh = false;
    int64_t retry_at_ms;
    xSemaphoreTake(s_ws_lock,portMAX_DELAY);
    session_present = s_ws != NULL;
    if (session_present && !s_ws_forced_dead) {
        session_usable = true;
        session_fresh = now_ms - s_ws_created_ms < WS_RECREATE_MIN_AGE_MS;
        session_alive = s_ws_connected && esp_websocket_client_is_connected(s_ws);
    }
    retry_at_ms = s_ws_retry_at_ms;
    xSemaphoreGive(s_ws_lock);
    if (session_usable && (session_fresh || session_alive)) {
        xSemaphoreGive(s_ws_lifecycle_lock);
        return ESP_OK;
    }
    if (!session_present && now_ms < retry_at_ms) {
        xSemaphoreGive(s_ws_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (session_present) {
        ws_epoch_advance();
        xSemaphoreTake(s_ws_lock,portMAX_DELAY);
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws=NULL;
        s_ws_connected=false;
        s_ws_forced_dead=false;
        ws_liveness_reset();
        xSemaphoreGive(s_ws_lock);
        ESP_LOGW(TAG, "dead long connection destroyed, refetching endpoint");
    }
    if (!s_event_q) {
        s_event_q = xQueueCreate(WS_EVENT_QUEUE_DEPTH, sizeof(ws_job_t));
        if (s_event_q && xTaskCreate(event_worker, "fs_event", FEISHU_EVENT_TASK_STACK_BYTES,
                                     NULL, 5, &s_event_task) != pdPASS) {
            ESP_LOGE(TAG, "fs_event task creation failed");
            vQueueDelete(s_event_q);
            s_event_q = NULL;
            s_event_task = NULL;
        }
    }
    if (!s_tx_q) {
        s_tx_q = xQueueCreate(WS_TX_QUEUE_DEPTH, sizeof(ws_tx_t));
        if (s_tx_q && xTaskCreate(ws_tx_task,"fs_tx",4096,NULL,5,&s_tx_task) != pdPASS) {
            vQueueDelete(s_tx_q); s_tx_q=NULL; s_tx_task=NULL;
        }
    }
    if (!s_event_q || !s_tx_q) { xSemaphoreGive(s_ws_lifecycle_lock); return ESP_ERR_NO_MEM; }

    /* The endpoint request may take up to 10 s (and every retry repeats it), so
     * it runs without the lifecycle lock: holding it here made every status
     * query report the long connection as offline and blocked feishu_ws_stop(). */
    xSemaphoreGive(s_ws_lifecycle_lock);

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "AppID", cfg.app_id);
    cJSON_AddStringToObject(root, "AppSecret", cfg.app_secret);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    char *resp = NULL;
    int status = 0;
    esp_err_t err = http_json(API_BASE WS_ENDPOINT_PATH, NULL, body, &resp, &status);
    free(body);
    if (err != ESP_OK || !resp) {
        ESP_LOGW(TAG, "ws endpoint request failed: %s http=%d", esp_err_to_name(err), status);
        free(resp);
        ws_retry_later(10000);
        return ESP_FAIL;
    }

    cJSON *j = cJSON_Parse(resp);
    cJSON *code = j ? cJSON_GetObjectItem(j, "code") : NULL;
    cJSON *data = j ? cJSON_GetObjectItem(j, "data") : NULL;
    cJSON *url = data ? cJSON_GetObjectItem(data, "URL") : NULL;
    cJSON *cc = data ? cJSON_GetObjectItem(data, "ClientConfig") : NULL;
    if (status != 200 || !cJSON_IsNumber(code) || code->valuedouble != 0 ||
        !cJSON_IsString(url) || strncmp(url->valuestring, "wss://", 6) != 0 ||
        strlen(url->valuestring) >= sizeof(s_ws_url)) {
        cJSON *msg = j ? cJSON_GetObjectItem(j, "msg") : NULL;
        ESP_LOGE(TAG, "ws endpoint response invalid (http=%d code=%ld msg=%s)", status,
                 (code && cJSON_IsNumber(code)) ? (long)code->valueint : -1L,
                 (msg && cJSON_IsString(msg)) ? msg->valuestring : "?");
        if (j) cJSON_Delete(j);
        free(resp);
        ws_retry_later(10000);
        return ESP_FAIL;
    }
    /* Parse into locals: this ran without the lifecycle lock, so writing the
     * shared session globals here could interleave with a second caller. */
    char url_local[sizeof(s_ws_url)];
    char conn_local[sizeof(s_conn_id)];
    int service_local = 0;
    int ping_local = 0;
    strlcpy(url_local, url->valuestring, sizeof(url_local));
    conn_local[0] = 0;
    if (cc) {
        cJSON *pi = cJSON_GetObjectItem(cc, "PingInterval");
        if (pi && pi->valueint > 0) ping_local = pi->valueint;
    }
    cJSON_Delete(j);
    free(resp);

    /* service_id / device_id live in the URL query. */
    char *q = strstr(url_local, "service_id=");
    if (q) service_local = atoi(q + strlen("service_id="));
    q = strstr(url_local, "device_id=");
    if (q) {
        const char *vp = q + strlen("device_id=");
        char *e = strchr(vp, '&');
        size_t n = e ? (size_t)(e - vp) : strlen(vp);
        if (n < sizeof(conn_local)) {
            memcpy(conn_local, vp, n);
            conn_local[n] = 0;
        }
    }

    xSemaphoreTake(s_ws_lifecycle_lock,portMAX_DELAY);
    xSemaphoreTake(s_lock,portMAX_DELAY);
    bool generation_current=(config_generation==s_cfg_generation);
    xSemaphoreGive(s_lock);
    if(!generation_current) {
        xSemaphoreGive(s_ws_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* Another caller may have installed a session while we fetched the endpoint.
     * Anything that is not provably dead is treated as "someone else's session":
     * overwriting it here used to leak an entire client (8 KB task + TLS context +
     * socket) while its handshake was still in flight, and two clients then fought
     * over one single-use endpoint.  Our freshly fetched URL is simply discarded;
     * the other session is expected to come up on its own. */
    xSemaphoreTake(s_ws_lock,portMAX_DELAY);
    bool other_session = s_ws && !s_ws_forced_dead;
    xSemaphoreGive(s_ws_lock);
    if (other_session) {
        ESP_LOGI(TAG, "ws session installed while fetching, keeping it");
        xSemaphoreGive(s_ws_lifecycle_lock);
        return ESP_OK;
    }

    esp_websocket_client_config_t wcfg = {
        .uri = url_local,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 8192,
        /* The event handler runs on this very task (no separate event task), so
         * the stack has to cover TLS + cJSON + logging. */
        .task_stack = 8192,
        .task_name = "ws_client",
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
        /* Feishu hands out a single-use URL: the driver's own reconnect loop
         * would keep retrying a URL the gateway already consumed and never
         * recover.  net_task refetches the endpoint instead, like the official
         * SDKs do. */
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
    };
    esp_websocket_client_handle_t client=esp_websocket_client_init(&wcfg);
    if (!client) {
        ws_retry_later(10000);
        xSemaphoreGive(s_ws_lifecycle_lock);
        return ESP_FAIL;
    }
    /* Publish the new session before the epoch moves.  The event handler drops
     * every event whose epoch does not match s_ws_epoch, so a CONNECTED event that
     * raced an epoch bump would be discarded and s_ws_connected would stay false
     * for up to WS_RECREATE_MIN_AGE_MS (20 s) on a socket that is actually up.
     * The handle becomes visible to other tasks only through s_ws_lock. */
    xSemaphoreTake(s_ws_lock,portMAX_DELAY);
    strlcpy(s_ws_url, url_local, sizeof(s_ws_url));
    strlcpy(s_conn_id, conn_local, sizeof(s_conn_id));
    s_service_id = service_local;
    if (ping_local > 0) s_ping_interval = ping_local;
    s_ws_cfg_generation=config_generation;
    s_ws=client;
    s_ws_created_ms=esp_timer_get_time()/1000;
    s_ws_retry_at_ms=0;
    s_ws_forced_dead=false;
    s_ws_connected=false;
    ws_liveness_reset();
    ws_epoch_advance();
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler,
                                  (void *)(uintptr_t)s_ws_epoch);
    xSemaphoreGive(s_ws_lock);
    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ws client start failed: %s", esp_err_to_name(err));
        ws_epoch_advance();
        xSemaphoreTake(s_ws_lock,portMAX_DELAY);
        esp_websocket_client_destroy(s_ws);
        s_ws=NULL;
        s_ws_connected=false;
        s_ws_forced_dead=false;
        ws_liveness_reset();
        s_ws_retry_at_ms=esp_timer_get_time()/1000+10000;
        xSemaphoreGive(s_ws_lock);
        xSemaphoreGive(s_ws_lifecycle_lock);
        return err;
    }
    if (!s_ping_task) {
        if (xTaskCreate(ping_task, "ws_ping", 3072, NULL, 4, &s_ping_task) != pdPASS) {
            /* Without this task nothing sends the application ping, so the session
             * would look application-dead to every liveness consumer. */
            ESP_LOGE(TAG, "ws_ping task creation failed, no application keepalive");
            s_ping_task = NULL;
        }
    }
    xSemaphoreGive(s_ws_lifecycle_lock);
    return ESP_OK;
}

static void ws_stop_locked(void)
{
    ws_epoch_advance();
    if (s_ws_lock) xSemaphoreTake(s_ws_lock,portMAX_DELAY);
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_ws_connected = false;
    s_ws_forced_dead = false;
    ws_liveness_reset();
    if (s_ws_lock) xSemaphoreGive(s_ws_lock);
}

void feishu_ws_stop(void)
{
    if(!s_ws_lifecycle_lock) s_ws_lifecycle_lock=xSemaphoreCreateMutex();
    if(!s_ws_lifecycle_lock) return;
    xSemaphoreTake(s_ws_lifecycle_lock,portMAX_DELAY);
    ws_stop_locked();
    xSemaphoreGive(s_ws_lifecycle_lock);
}

bool feishu_ws_connected(void) { return s_ws_connected && !s_ws_forced_dead; }

int64_t feishu_ws_rx_age_ms(void)
{
    if (!s_ws_connected) return -1;
    /* Application-level age, not transport age: the value the callers can act on
     * is "how long has the Feishu session been silent", and the CONNECTED instant
     * stands in for the first frame a fresh session has not delivered yet. */
    int64_t last = ws_app_reference_ms();
    if (last <= 0) return -1;
    int64_t now = esp_timer_get_time() / 1000;
    return now > last ? now - last : 0;
}

int64_t feishu_ws_connected_age_ms(void)
{
    int64_t since;
    portENTER_CRITICAL(&s_ws_liveness_mux);
    since = s_ws_connected_ms;
    portEXIT_CRITICAL(&s_ws_liveness_mux);
    if (!s_ws_connected || since <= 0) return -1;
    int64_t now = esp_timer_get_time() / 1000;
    return now > since ? now - since : 0;
}

bool feishu_ws_alive(void)
{
    if (!feishu_ws_connected()) return false;
    int64_t age = feishu_ws_rx_age_ms();
    /* No measurement yet (CONNECTED not processed): do not invent a failure. */
    if (age < 0) return true;
    return age <= ws_app_stale_limit_ms();
}

bool feishu_ws_force_reconnect(void)
{
    if (!s_ws_connected) {
        /* Callable without a live socket - the repair ladder may have spent a
         * cooldown before noticing the session is already gone - but never silent:
         * a no-op that looks like a successful repair is how a wedged session
         * survives. */
        ESP_LOGW(TAG, "force reconnect requested but no session is up");
        return false;
    }
    s_ws_forced_dead = true;
    /* Say *why* it looks dead: "nothing at all" is a transport problem, while
     * "transport frames but no application frames" is the wedged Feishu session
     * this whole freshness signal exists for. */
    int64_t ctrl, app, now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_ws_liveness_mux);
    ctrl = s_ws_last_ctrl_ms;
    app = s_ws_last_app_ms;
    portEXIT_CRITICAL(&s_ws_liveness_mux);
    ESP_LOGW(TAG, "long connection looks dead (last frame %lld ms ago, last application "
                  "frame %lld ms ago), endpoint will be refetched",
             (long long)(ctrl > 0 ? now - ctrl : -1), (long long)(app > 0 ? now - app : -1));
    return true;
}
int feishu_ws_reconnects(void) { return s_ws_reconnects; }

bool feishu_ws_authenticated_for(const char *app_id, const char *app_secret)
{
    if (!s_ws_lifecycle_lock || !app_id || !*app_id || !app_secret || !*app_secret ||
        xSemaphoreTake(s_ws_lifecycle_lock, 0) != pdTRUE) return false;
    bool online = s_ws_connected && s_ws_online_epoch == s_ws_epoch &&
                  s_ws_cfg_generation == s_cfg_generation &&
                  strcmp(s_cfg.app_id, app_id) == 0 && strcmp(s_cfg.app_secret, app_secret) == 0 &&
                  s_ws && esp_websocket_client_is_connected(s_ws);
    xSemaphoreGive(s_ws_lifecycle_lock);
    return online;
}

static void learn_peer_for_generation(const char *open_id, const char *chat_id, uint32_t generation)
{
    if (!open_id || !open_id[0]) return;
    bool changed = false;
    if (s_lock) xSemaphoreTake(s_lock,portMAX_DELAY);
    if (generation != s_cfg_generation ||
        (s_cfg.open_id[0] && strcmp(s_cfg.open_id,open_id)!=0) ||
        (!s_cfg.open_id[0] && s_cfg.chat_id[0] && chat_id && strcmp(s_cfg.chat_id,chat_id)!=0)) {
        if (s_lock) xSemaphoreGive(s_lock);
        return;
    }
    if (strcmp(s_cfg.open_id, open_id) != 0) {
        strlcpy(s_cfg.open_id, open_id, sizeof(s_cfg.open_id));
        ESP_LOGI(TAG, "learned direct-message target");
        changed = true;
    }
    if (chat_id && chat_id[0] && strcmp(s_cfg.chat_id, chat_id) != 0) {
        strlcpy(s_cfg.chat_id, chat_id, sizeof(s_cfg.chat_id));
        changed = true;
    }
    char saved_open[sizeof(s_cfg.open_id)], saved_chat[sizeof(s_cfg.chat_id)];
    strlcpy(saved_open,s_cfg.open_id,sizeof(saved_open)); strlcpy(saved_chat,s_cfg.chat_id,sizeof(saved_chat));
    feishu_peer_cb_t cb=s_peer_cb;
    if (s_lock) xSemaphoreGive(s_lock);
    if (changed && cb) cb(saved_open,saved_chat);
}

void feishu_learn_peer(const char *open_id, const char *chat_id)
{
    learn_peer_for_generation(open_id,chat_id,s_cfg_generation);
}

void feishu_set_peer_cb(feishu_peer_cb_t cb)
{
    s_peer_cb = cb;
}

bool feishu_configured(void)
{
    return s_cfg.app_id[0] && s_cfg.app_secret[0];
}

esp_err_t feishu_init(const feishu_cfg_t *cfg)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_ws_lock) s_ws_lock = xSemaphoreCreateMutex();
    if (!s_ws_lifecycle_lock) s_ws_lifecycle_lock=xSemaphoreCreateMutex();
    if (!s_token_lock) s_token_lock=xSemaphoreCreateMutex();
    if (cfg) {
        if(!s_lock || !s_ws_lock || !s_ws_lifecycle_lock || !s_token_lock) return ESP_ERR_NO_MEM;
        xSemaphoreTake(s_ws_lifecycle_lock,portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool app_changed = s_cfg_initialized &&
                           feishu_credentials_changed(s_cfg_initialized, s_cfg.app_id,
                                                      s_cfg.app_secret, cfg->app_id, cfg->app_secret);
        s_cfg_generation++;
        s_cfg = *cfg;
        /* Invalidate the cached token without taking s_token_lock: token_get() holds
         * that lock across a 10 s HTTP refresh, so nesting it under s_lock here made
         * saving the configuration in the web UI block net_task's reconnect for the
         * whole request.  Bumping the epoch is enough - token_get() treats a token
         * filled for an older epoch as absent. */
        s_token_epoch++;
        if (app_changed) {
            s_cfg.open_id[0] = 0; s_cfg.chat_id[0] = 0;
        }
        s_cfg_initialized = true;
        xSemaphoreGive(s_lock);
        /* Also clears the connected flag and the two liveness stamps. */
        ws_stop_locked();
        xSemaphoreGive(s_ws_lifecycle_lock);
        if (app_changed && s_peer_cb) s_peer_cb("", "");
    }
    return ESP_OK;
}

void feishu_set_cmd_cb(feishu_cmd_cb_t cb)
{
    s_cmd_cb = cb;
}
