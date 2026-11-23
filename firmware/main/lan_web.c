#include "lan_web.h"

#include "factory_reset.h"
#include "firmware_update.h"
#include "rpc.h"
#include "wifi.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "lan_web";
static httpd_handle_t s_server;
static atomic_bool s_available;
static SemaphoreHandle_t s_lock;
static char s_token[33];

extern const char _binary_index_html_start[];
extern const char _binary_index_html_end[];

#define LAN_RPC_BODY_MAX 2048
#define LAN_RPC_RESPONSE_MAX 8192
#define LAN_AUTH_HEADER_MAX 64

static void set_security_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
        "connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
}

static esp_err_t send_json_status(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    set_security_headers(req);
    return httpd_resp_sendstr(req, json);
}

static esp_err_t page_get(httpd_req_t *req)
{
    const size_t page_len = (size_t)(_binary_index_html_end - _binary_index_html_start - 1);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    set_security_headers(req);
    return httpd_resp_send(req, _binary_index_html_start, page_len);
}

static esp_err_t api_info_get(httpd_req_t *req)
{
    return send_json_status(req, "200 OK", "{\"transport\":\"lan\"}");
}

static esp_err_t rpc_method_not_allowed(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Allow", "POST");
    return send_json_status(req, "405 Method Not Allowed", "{\"ok\":false,\"err\":\"method_not_allowed\"}");
}

static bool request_same_origin(httpd_req_t *req)
{
    size_t origin_len = httpd_req_get_hdr_value_len(req, "Origin");
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    if (!origin_len || !host_len || origin_len >= 96 || host_len >= 80) return false;

    char origin[96], host[80];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
        return false;
    static const char prefix[] = "http://";
    if (strncmp(origin, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *origin_authority = origin + sizeof(prefix) - 1;
    char ssid[40] = {0}, ip[20] = {0}, host_with_port[28] = {0};
    wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), NULL);
    if (!ip[0]) return false;
    snprintf(host_with_port, sizeof(host_with_port), "%s:80", ip);
    bool current_device_host = strcmp(host, ip) == 0 || strcmp(host, host_with_port) == 0;
    return current_device_host && origin_authority[0] && strcmp(origin_authority, host) == 0;
}

static bool request_has_token(httpd_req_t *req)
{
    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (auth_len != 39 || !s_lock) return false; /* "Bearer " + 128-bit hex token */
    char auth[LAN_AUTH_HEADER_MAX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
        memcmp(auth, "Bearer ", 7) != 0)
        return false;

    uint8_t diff = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool token_created = s_token[0] != '\0';
    for (size_t i = 0; i < 32; ++i) diff |= (uint8_t)(auth[i + 7] ^ s_token[i]);
    xSemaphoreGive(s_lock);
    return token_created && diff == 0;
}

static void *alloc_io_buffer(size_t len)
{
    void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(len);
    return p;
}

static esp_err_t rpc_post(httpd_req_t *req)
{
    if (!request_same_origin(req))
        return send_json_status(req, "403 Forbidden", "{\"ok\":false,\"err\":\"same_origin_required\"}");
    if (!request_has_token(req))
        return send_json_status(req, "401 Unauthorized", "{\"ok\":false,\"err\":\"unauthorized\"}");

    size_t content_type_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    char content_type[48] = {0};
    if (!content_type_len || content_type_len >= sizeof(content_type) ||
        httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';' && content_type[16] != ' '))
        return send_json_status(req, "415 Unsupported Media Type", "{\"ok\":false,\"err\":\"json_required\"}");

    if (req->content_len == 0 || req->content_len > LAN_RPC_BODY_MAX)
        return send_json_status(req, "413 Content Too Large", "{\"ok\":false,\"err\":\"request_too_large\"}");

    char *body = alloc_io_buffer(req->content_len + 1);
    char *response = alloc_io_buffer(LAN_RPC_RESPONSE_MAX);
    if (!body || !response) {
        free(body);
        free(response);
        return send_json_status(req, "503 Service Unavailable", "{\"ok\":false,\"err\":\"memory\"}");
    }

    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            free(body);
            free(response);
            return send_json_status(req, "408 Request Timeout", "{\"ok\":false,\"err\":\"body_incomplete\"}");
        }
        received += (size_t)n;
    }
    body[received] = '\0';
    response[0] = '\0';
    rpc_post_reply_action_t action = RPC_POST_NONE;
    esp_err_t result = rpc_dispatch_json(body, response, LAN_RPC_RESPONSE_MAX, false, &action);
    free(body);
    if (response[0] == '\0') {
        free(response);
        return send_json_status(req, "500 Internal Server Error", "{\"ok\":false,\"err\":\"rpc_failed\"}");
    }
    (void)result; /* RPC command errors are returned in its JSON body. */

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    set_security_headers(req);
    esp_err_t sent = httpd_resp_sendstr(req, response);
    free(response);
    /* RPC already committed persistent state. Run its deferred effect even if
       the peer closed early, so saved settings do not wait for reboot to apply. */
    if (action != RPC_POST_NONE) rpc_run_post_reply_action(action);
    return sent;
}

typedef struct {
    httpd_req_t *request;
    int64_t deadline_us;
} upload_request_t;

static int read_firmware(void *ctx, void *buf, size_t len)
{
    const upload_request_t *upload = ctx;
    if (esp_timer_get_time() >= upload->deadline_us) return -1;
    return httpd_req_recv(upload->request, buf, len);
}

static esp_err_t send_upload_error(httpd_req_t *req, const char *status, const char *json)
{
    /* Close instead of draining a rejected, potentially oversized request body. */
    httpd_resp_set_hdr(req, "Connection", "close");
    send_json_status(req, status, json);
    return ESP_FAIL;
}

static esp_err_t firmware_post(httpd_req_t *req)
{
    if (!request_same_origin(req))
        return send_upload_error(req, "403 Forbidden", "{\"ok\":false,\"err\":\"same_origin_required\"}");
    if (!request_has_token(req))
        return send_upload_error(req, "401 Unauthorized", "{\"ok\":false,\"err\":\"unauthorized\"}");

    char content_type[32];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcasecmp(content_type, "application/octet-stream") != 0 ||
        httpd_req_get_hdr_value_len(req, "Transfer-Encoding") ||
        httpd_req_get_hdr_value_len(req, "Content-Encoding"))
        return send_upload_error(req, "415 Unsupported Media Type", "{\"ok\":false,\"err\":\"octet_stream_required\"}");

    static const struct {
        const char *status;
        const char *name;
    } errors[] = {
        [FIRMWARE_BUSY] = {"409 Conflict", "firmware_busy"},
        [FIRMWARE_UNAVAILABLE] = {"409 Conflict", "firmware_unavailable"},
        [FIRMWARE_SIZE] = {"413 Content Too Large", "firmware_size"},
        [FIRMWARE_PACKAGE] = {"400 Bad Request", "invalid_package"},
        [FIRMWARE_BOARD_MISMATCH] = {"400 Bad Request", "wrong_board"},
        [FIRMWARE_IMAGE] = {"400 Bad Request", "invalid_image"},
        [FIRMWARE_CHECKSUM] = {"400 Bad Request", "checksum_mismatch"},
        [FIRMWARE_INCOMPLETE] = {"408 Request Timeout", "body_incomplete"},
        [FIRMWARE_WRITE] = {"500 Internal Server Error", "firmware_write"},
        [FIRMWARE_SELECT] = {"500 Internal Server Error", "firmware_select"},
        [FIRMWARE_MEMORY] = {"503 Service Unavailable", "memory"},
    };
    upload_request_t upload = { .request = req, .deadline_us = esp_timer_get_time() + 90000000 };
    char version[FIRMWARE_VERSION_SIZE];
    firmware_result_t result = firmware_upload(req->content_len, read_firmware, &upload, version);
    if (result != FIRMWARE_OK) {
        char body[64];
        snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\"}", errors[result].name);
        return send_upload_error(req, errors[result].status, body);
    }

    /* The validator allows only printable ASCII. Escape JSON's two special characters. */
    char escaped[2 * FIRMWARE_VERSION_SIZE];
    size_t n = 0;
    for (const char *p = version; *p; ++p) {
        if (*p == '"' || *p == '\\') escaped[n++] = '\\';
        escaped[n++] = *p;
    }
    escaped[n] = '\0';
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"version\":\"%s\"}", escaped);

    esp_err_t sent = send_json_status(req, "200 OK", body);
    rpc_run_post_reply_action(RPC_POST_RESTART);
    return sent;
}

static bool create_token_locked(void)
{
    if (s_token[0]) return true;
    uint8_t raw[16];
    static const char hex[] = "0123456789abcdef";
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); ++i) {
        s_token[i * 2] = hex[raw[i] >> 4];
        s_token[i * 2 + 1] = hex[raw[i] & 0x0f];
    }
    s_token[32] = '\0';
    return true;
}

esp_err_t lan_web_start(void)
{
    if (!wifi_connected()) return ESP_ERR_INVALID_STATE;
    esp_err_t rpc_err = rpc_init();
    if (rpc_err != ESP_OK) return rpc_err;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool already_started = s_server != NULL;
    xSemaphoreGive(s_lock);
    if (already_started) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 6;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server start failed (%s)", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = page_get };
    const httpd_uri_t index = { .uri = "/index.html", .method = HTTP_GET, .handler = page_get };
    const httpd_uri_t info = { .uri = "/api/info", .method = HTTP_GET, .handler = api_info_get };
    const httpd_uri_t rpc = { .uri = "/api/rpc", .method = HTTP_POST, .handler = rpc_post };
    const httpd_uri_t upload = { .uri = "/api/firmware", .method = HTTP_POST, .handler = firmware_post };
    const httpd_uri_t rpc_get = { .uri = "/api/rpc", .method = HTTP_GET, .handler = rpc_method_not_allowed };
    if ((err = httpd_register_uri_handler(server, &page)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &index)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &info)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &rpc)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &upload)) != ESP_OK ||
        (err = httpd_register_uri_handler(server, &rpc_get)) != ESP_OK) {
        httpd_stop(server);
        ESP_LOGE(TAG, "route registration failed (%s)", esp_err_to_name(err));
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_server) {
        s_server = server;
        atomic_store(&s_available, true);
    } else {
        xSemaphoreGive(s_lock);
        httpd_stop(server);
        return ESP_OK;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "management server started");
    return ESP_OK;
}

void lan_web_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    httpd_handle_t server = s_server;
    s_server = NULL;
    atomic_store(&s_available, false);
    xSemaphoreGive(s_lock);
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "management server stopped");
    }
}

bool lan_web_ready(void)
{
    return atomic_load(&s_available) && wifi_connected();
}

bool lan_web_management_url(char *out, size_t cap)
{
    if (!out || !cap || factory_reset_pending() || !wifi_connected() || !s_lock) return false;
    char ssid[40] = {0}, ip[20] = {0};
    wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), NULL);
    if (!ip[0]) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_server) {
        xSemaphoreGive(s_lock);
        return false;
    }
    bool token_ok = create_token_locked();
    int n = token_ok ? snprintf(out, cap, "http://%s/#token=%s", ip, s_token) : -1;
    xSemaphoreGive(s_lock);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}
