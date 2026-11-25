#include "mms_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mms_policy.h"

#define MMS_CID 5
#define MMS_SOCKET 5
#define MMS_READ_CHUNK 1024
#define MMS_CONNECT_TIMEOUT_MS 30000
#define MMS_TOTAL_TIMEOUT_MS (10 * 60 * 1000)
#define MMS_AT_TIMEOUT_MS 6000
#define MMS_HOME_POLL_MS 1000
#define MMS_CID_COUNT 2

static const char *TAG = "mms_fetch";

typedef struct {
    uint32_t available;
    int call_states[6];
    uint8_t call_seen;
    int socket_state;
    int registration_status;
    int open_result; /* -1 pending, 0 connected, positive modem error */
    unsigned send_result; /* byte count of the last MIPSEND confirmation */
    bool disconnected;
    bool data_lost;
    bool active;
} mms_urc_state_t;

typedef enum { MMS_GATE_OK, MMS_GATE_TEMPORARY, MMS_GATE_BLOCKED } mms_gate_t;

/* Contexts outlive a fetch because RX can already have copied the observer. */
static mms_urc_state_t s_urc_state[MMS_CID_COUNT];
static int cereg_status(const char *response);

static bool parse_uint(const char **p, unsigned *out)
{
    if (!p || !*p || !out || **p < '0' || **p > '9') return false;
    unsigned n = 0;
    do {
        unsigned d = (unsigned)(**p - '0');
        if (n > (UINT32_MAX - d) / 10) return false;
        n = n * 10 + d;
        ++*p;
    } while (**p >= '0' && **p <= '9');
    *out = n;
    return true;
}

static bool field_uint(const char **p, unsigned *out, char delimiter)
{
    if (!parse_uint(p, out) || **p != delimiter) return false;
    ++*p;
    return true;
}

static void mms_urc(modem_t *m, const char *line, void *ctx)
{
    mms_urc_state_t *s = (mms_urc_state_t *)ctx;
    if (!m || !s || !line) return;

    bool wake = false;
    taskENTER_CRITICAL(&m->rx_mux);
    if (!s->active) {
        taskEXIT_CRITICAL(&m->rx_mux);
        return;
    }
    if (strncmp(line, "+MIPCALL:", 9) == 0) {
        const char *p = line + 9;
        unsigned cid, active;
        while (*p == ' ') ++p;
        if (field_uint(&p, &cid, ',') && cid < 6 && parse_uint(&p, &active)) {
            s->call_seen |= (uint8_t)(1u << cid);
            s->call_states[cid] = active == 1 ? 1 : 0;
            if (cid == MMS_CID && active != 1 && s->open_result >= 0) s->data_lost = true;
            wake = true;
        }
    } else if (strncmp(line, "+MIPSEND:", 9) == 0) {
        const char *p = line + 9;
        unsigned id, n;
        while (*p == ' ') ++p;
        if (field_uint(&p, &id, ',') && id == MMS_SOCKET && parse_uint(&p, &n)) {
            s->send_result = n;
            wake = true;
        }
    } else if (strncmp(line, "+MIPOPEN:", 9) == 0) {
        const char *p = line + 9;
        unsigned id, result;
        while (*p == ' ') ++p;
        if (field_uint(&p, &id, ',') && id == MMS_SOCKET && parse_uint(&p, &result)) {
            s->open_result = (int)result;
            wake = true;
        }
    } else if (strncmp(line, "+MIPURC:", 8) == 0) {
        const char *p = line + 8;
        while (*p == ' ') ++p;
        if (*p++ == '"') {
            const char *kind = p;
            const char *end = strchr(kind, '"');
            if (end) {
                p = end + 1;
                while (*p == ' ') ++p;
                if (*p++ == ',') {
                    unsigned id;
                    if (parse_uint(&p, &id) && id == MMS_SOCKET) {
                        if ((size_t)(end - kind) == 4 && !memcmp(kind, "rtcp", 4)) {
                            unsigned recv_n, total_n;
                            if (*p++ == ',' && parse_uint(&p, &recv_n) && *p++ == ',' &&
                                parse_uint(&p, &total_n)) {
                                (void)recv_n;
                                s->available = total_n;
                                wake = true;
                            } else {
                                s->data_lost = true;
                                wake = true;
                            }
                        } else if ((size_t)(end - kind) == 7 && !memcmp(kind, "disconn", 7)) {
                            s->disconnected = true;
                            wake = true;
                        } else if ((size_t)(end - kind) == 4 && !memcmp(kind, "drop", 4)) {
                            s->data_lost = true;
                            wake = true;
                        }
                    }
                }
            }
        }
    } else if (strncmp(line, "+MIPCLOSE:", 10) == 0) {
        const char *p = line + 10;
        unsigned id;
        while (*p == ' ') ++p;
        if (parse_uint(&p, &id) && id == MMS_SOCKET) {
            s->disconnected = true;
            wake = true;
        }
    } else if (strncmp(line, "+MIPSTATE:", 10) == 0) {
        const char *p = line + 10;
        unsigned id;
        while (*p == ' ') ++p;
        if (field_uint(&p, &id, ',') && id == MMS_SOCKET) {
            const char *last = strrchr(p, ',');
            if (last) {
                ++last;
                while (*last == ' ' || *last == '\t') ++last;
                if (*last == '"') ++last;
                const char *end = last + strcspn(last, "\" \t\r\n");
                s->socket_state = ((size_t)(end - last) == 7 && !memcmp(last, "INITIAL", 7)) ? 0 : 1;
                wake = true;
            }
        }
    } else if (strncmp(line, "+CEREG:", 7) == 0) {
        int status = cereg_status(line);
        if (status >= 0) {
            s->registration_status = status;
            if (status != 1) s->data_lost = true;
            wake = true;
        } else {
            /* A malformed registration report during fetch is fail-closed. */
            s->data_lost = true;
            wake = true;
        }
    }
    taskEXIT_CRITICAL(&m->rx_mux);
    (void)wake; /* Fetch waits by bounded polling; no task handle crosses callbacks. */
}

static int cereg_status(const char *response)
{
    const char *p = strstr(response, "+CEREG:");
    if (!p || (p != response && p[-1] != '\n' && p[-1] != '\r')) return -1;
    p += 7;
    while (*p == ' ') ++p;
    unsigned first;
    if (!parse_uint(&p, &first)) return -1;
    if (*p == ',') {
        ++p;
        while (*p == ' ') ++p;
        unsigned stat;
        if (!parse_uint(&p, &stat) || (*p && *p != ',' && *p != '\r' && *p != '\n')) return -1;
        return (int)stat;
    }
    if (*p && *p != '\r' && *p != '\n') return -1;
    return (int)first;
}

static mms_gate_t home_check(modem_t *m, const mms_profile_t *profile,
                             char *resp, size_t cap)
{
    if (modem_cmd(m, "AT+CEREG?", resp, cap, 2500) != ESP_OK) return MMS_GATE_TEMPORARY;
    int status = cereg_status(resp);
    if (status == 5) return MMS_GATE_BLOCKED;
    if (!mms_home_registered(resp)) return MMS_GATE_TEMPORARY;
    if (modem_cmd(m, "AT+COPS?", resp, cap, 3000) != ESP_OK) return MMS_GATE_TEMPORARY;
    if (!mms_serving_home(profile, resp)) return MMS_GATE_BLOCKED;
    if (modem_cmd(m, "AT+CIMI", resp, cap, 3500) != ESP_OK) return MMS_GATE_TEMPORARY;
    const mms_profile_t *current = mms_profile_from_imsi(resp);
    if (!current || current != profile) return MMS_GATE_BLOCKED;
    return MMS_GATE_OK;
}

static int cereg_mode(const char *response)
{
    const char *p = strstr(response, "+CEREG:");
    if (!p || (p != response && p[-1] != '\r' && p[-1] != '\n')) return -1;
    p += 7;
    while (*p == ' ') ++p;
    unsigned n;
    return parse_uint(&p, &n) && n <= 5 ? (int)n : -1;
}

static int cops_format(const char *response)
{
    const char *p = strstr(response, "+COPS:");
    if (!p || (p != response && p[-1] != '\r' && p[-1] != '\n')) return -1;
    int mode = -1, format = -1;
    if (sscanf(p, "+COPS: %d , %d", &mode, &format) != 2 || format < 0 || format > 2)
        return -1;
    return format;
}

static mms_gate_t state_gate(modem_t *m)
{
    mms_urc_state_t *s = &s_urc_state[m->idx];
    taskENTER_CRITICAL(&m->rx_mux);
    int reg = s->registration_status;
    bool lost = s->data_lost;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (reg == 5) return MMS_GATE_BLOCKED;
    return lost ? MMS_GATE_TEMPORARY : MMS_GATE_OK;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_read_reply(const char *resp, unsigned *unread,
                             unsigned *actual, const char **hex)
{
    const char *p = strstr(resp, "+MIPRD:");
    if (!p || (p != resp && p[-1] != '\n' && p[-1] != '\r')) return false;
    p += 7;
    while (*p == ' ') ++p;
    unsigned id, left, n;
    if (!field_uint(&p, &id, ',') || id != MMS_SOCKET ||
        !field_uint(&p, &left, ',') ||
        !field_uint(&p, &n, ',')) return false;
    if (*p == ' ') ++p;
    const char *end = p + strcspn(p, "\r\n");
    if ((size_t)(end - p) != (size_t)n * 2) return false;
    for (const char *q = p; q < end; ++q) if (hex_value(*q) < 0) return false;
    *unread = left;
    *actual = n;
    *hex = p;
    return true;
}

static bool query_unread(modem_t *m, unsigned *unread, char *resp, size_t cap)
{
    if (modem_cmd(m, "AT+MIPRD=5", resp, cap, MMS_AT_TIMEOUT_MS) != ESP_OK) return false;
    const char *p = strstr(resp, "+MIPRD:");
    if (!p || (p != resp && p[-1] != '\r' && p[-1] != '\n')) return false;
    p += 7;
    while (*p == ' ') ++p;
    unsigned id;
    return field_uint(&p, &id, ',') && id == MMS_SOCKET && parse_uint(&p, unread) &&
           (*p == '\0' || *p == '\r' || *p == '\n');
}

static bool decode_hex_append(uint8_t *dst, size_t cap, size_t *used,
                              const char *hex, size_t bytes)
{
    if (bytes > (cap - *used) || bytes > (SIZE_MAX / 2)) return false;
    for (size_t i = 0; i < bytes; ++i) {
        int hi = hex_value(hex[i * 2]), lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        dst[(*used)++] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool send_request(modem_t *m, const mms_profile_t *profile,
                         const uint8_t *request, size_t length,
                         char *resp, size_t cap, mms_urc_state_t *state,
                         mms_gate_t *gate_out)
{
    *gate_out = MMS_GATE_OK;
    size_t off = 0;
    while (off < length) {
        *gate_out = home_check(m, profile, resp, cap);
        if (*gate_out != MMS_GATE_OK) return false;
        *gate_out = state_gate(m);
        if (*gate_out != MMS_GATE_OK) return false;
        size_t n = length - off;
        if (n > 120) n = 120; /* encoded AT line remains below 320 bytes */
        char cmd[320];
        int head = snprintf(cmd, sizeof(cmd), "AT+MIPSEND=%d,%u,\"", MMS_SOCKET, (unsigned)n);
        if (head < 0 || (size_t)head + n * 2 + 3 > sizeof(cmd)) return false;
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < n; ++i) {
            cmd[head + i * 2] = hex[request[off + i] >> 4];
            cmd[head + i * 2 + 1] = hex[request[off + i] & 15];
        }
        cmd[head + n * 2] = '"';
        cmd[head + n * 2 + 1] = '\0';
        taskENTER_CRITICAL(&m->rx_mux);
        state->send_result = 0;
        taskEXIT_CRITICAL(&m->rx_mux);
        if (modem_cmd(m, cmd, resp, cap, MMS_AT_TIMEOUT_MS) != ESP_OK) return false;
        taskENTER_CRITICAL(&m->rx_mux);
        unsigned sent = state->send_result;
        taskEXIT_CRITICAL(&m->rx_mux);
        if (sent != n) return false;
        off += n;
    }
    return true;
}

static bool read_context(modem_t *m, char *type, size_t type_cap,
                         char *apn, size_t apn_cap, bool *found,
                         char *resp, size_t cap)
{
    *found = false;
    if (modem_cmd(m, "AT+CGDCONT?", resp, cap, 5000) != ESP_OK) return false;
    const char *p = resp;
    while ((p = strstr(p, "+CGDCONT:")) != NULL) {
        if (p != resp && p[-1] != '\n' && p[-1] != '\r') { p += 9; continue; }
        p += 9;
        while (*p == ' ') ++p;
        unsigned cid;
        if (!field_uint(&p, &cid, ',')) continue;
        if (cid != MMS_CID) continue;
        while (*p == ' ') ++p;
        if (*p++ != '"') return false;
        const char *te = strchr(p, '"');
        if (!te || (size_t)(te - p) >= type_cap) return false;
        memcpy(type, p, te - p); type[te - p] = 0;
        p = te + 1;
        while (*p == ' ') ++p;
        if (*p++ != ',') return false;
        while (*p == ' ') ++p;
        if (*p++ != '"') return false;
        const char *ae = strchr(p, '"');
        if (!ae || (size_t)(ae - p) >= apn_cap) return false;
        memcpy(apn, p, ae - p); apn[ae - p] = 0;
        *found = true;
        return true;
    }
    return true;
}

static bool query_socket_idle(modem_t *m, mms_urc_state_t *state,
                              char *resp, size_t cap)
{
    taskENTER_CRITICAL(&m->rx_mux);
    state->socket_state = -1;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (modem_cmd(m, "AT+MIPSTATE=5", resp, cap, 3000) != ESP_OK) return false;
    int64_t until = esp_timer_get_time() + 1200000;
    while (esp_timer_get_time() < until) {
        taskENTER_CRITICAL(&m->rx_mux);
        int status = state->socket_state;
        taskEXIT_CRITICAL(&m->rx_mux);
        if (status >= 0) return status == 0;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false; /* Unknown must never be treated as idle. */
}

static bool query_call_idle(modem_t *m, mms_urc_state_t *state,
                            char *resp, size_t cap)
{
    taskENTER_CRITICAL(&m->rx_mux);
    state->call_seen = 0;
    for (size_t i = 0; i < 6; ++i) state->call_states[i] = -1;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (modem_cmd(m, "AT+MIPCALL?", resp, cap, 4000) != ESP_OK) return false;
    taskENTER_CRITICAL(&m->rx_mux);
    uint8_t seen = state->call_seen;
    int cid1 = state->call_states[1];
    int cid5 = state->call_states[MMS_CID];
    taskEXIT_CRITICAL(&m->rx_mux);
    /* Only already active contexts are listed by MIPCALL?. Default CID 1
     * must be present, and the MMS dedicated CID must be absent. */
    return (seen & (1u << 1)) && cid1 == 1 &&
           (!(seen & (1u << MMS_CID)) || cid5 != 1);
}

static bool query_mipcfg(modem_t *m, const char *name, unsigned *first,
                         unsigned *second, char *resp, size_t cap)
{
    char cmd[64], prefix[40];
    snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"%s\",5", name);
    if (modem_cmd(m, cmd, resp, cap, 3000) != ESP_OK) return false;
    snprintf(prefix, sizeof(prefix), "+MIPCFG: \"%s\",5,", name);
    const char *p = strstr(resp, prefix);
    if (!p || (p != resp && p[-1] != '\r' && p[-1] != '\n')) return false;
    p += strlen(prefix);
    if (!parse_uint(&p, first)) return false;
    if (second && (*p++ != ',' || !parse_uint(&p, second))) return false;
    return *p == '\0' || *p == '\r' || *p == '\n';
}

static bool restore_context(modem_t *m, const char *type, const char *apn,
                            bool found, char *resp, size_t cap)
{
    if (!found) {
        /* 3GPP AT+CGDCONT=<cid> deletes a context definition. */
        char del[32];
        snprintf(del, sizeof(del), "AT+CGDCONT=%d", MMS_CID);
        return modem_cmd(m, del, resp, cap, MMS_AT_TIMEOUT_MS) == ESP_OK;
    }
    char cmd[256];
    int n = snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"%s\",\"%s\"", MMS_CID,
                     found ? type : "IPV4V6", found ? apn : "");
    return n > 0 && (size_t)n < sizeof(cmd) &&
           modem_cmd(m, cmd, resp, cap, MMS_AT_TIMEOUT_MS) == ESP_OK;
}

static bool restore_mipcfg(modem_t *m, unsigned cid, unsigned tx_format,
                           unsigned rx_format, unsigned ssl_enable, unsigned ssl_id,
                           char *resp, size_t cap)
{
    char cmd[96];
    int n = snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"cid\",5,%u", cid);
    if (n <= 0 || (size_t)n >= sizeof(cmd) || modem_cmd(m, cmd, resp, cap, MMS_AT_TIMEOUT_MS) != ESP_OK)
        return false;
    n = snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"encoding\",5,%u,%u", tx_format, rx_format);
    if (n <= 0 || (size_t)n >= sizeof(cmd) || modem_cmd(m, cmd, resp, cap, MMS_AT_TIMEOUT_MS) != ESP_OK)
        return false;
    n = snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"ssl\",5,%u,%u", ssl_enable, ssl_id);
    return n > 0 && (size_t)n < sizeof(cmd) && modem_cmd(m, cmd, resp, cap, MMS_AT_TIMEOUT_MS) == ESP_OK;
}

esp_err_t mms_fetch(modem_t *m, const char *url, uint8_t **body, size_t *len)
{
    if (body) *body = NULL;
    if (len) *len = 0;
    if (!m || !url || !body || !len || m->idx < 0 || m->idx >= MMS_CID_COUNT || !m->mms_lock)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(m->mms_lock, 0) != pdTRUE) return MODEM_ERR_BUSY;

    char resp[2304] = {0};
    char request[1200] = {0};
    char old_type[32] = {0}, old_apn[128] = {0};
    bool old_context_found = false;
    bool context_changed = false, call_started = false, socket_opened = false;
    bool socket_connected = false, cfg_changed = false, cereg_changed = false, cops_changed = false;
    unsigned old_socket_cid = 0, old_tx_format = 0, old_rx_format = 0;
    unsigned old_ssl_enable = 0, old_ssl_id = 0;
    int old_cereg_mode = -1, old_cops_format = -1;
    bool result_ok = false, eof = false;
    uint8_t *wire = NULL;
    size_t wire_len = 0, decoded_len = 0;
    esp_err_t result = ESP_FAIL;
    mms_urc_state_t *state = &s_urc_state[m->idx];
    taskENTER_CRITICAL(&m->rx_mux);
    memset(state, 0, sizeof(*state));
    state->open_result = -1;
    state->socket_state = -1;
    state->registration_status = -1;
    for (size_t i = 0; i < 6; ++i) state->call_states[i] = -1;
    state->active = true;
    taskEXIT_CRITICAL(&m->rx_mux);
    modem_set_observer(m, mms_urc, state);

    if (modem_cmd(m, "AT+CIMI", resp, sizeof(resp), 3500) != ESP_OK) goto cleanup;
    const mms_profile_t *profile = mms_profile_from_imsi(resp);
    if (!profile) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    if (modem_cmd(m, "AT+CEREG?", resp, sizeof(resp), 2500) != ESP_OK) goto cleanup;
    old_cereg_mode = cereg_mode(resp);
    if (old_cereg_mode < 0) goto cleanup;
    if (!mms_home_registered(resp)) {
        if (cereg_status(resp) == 5) result = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    if (modem_cmd(m, "AT+COPS?", resp, sizeof(resp), 3000) != ESP_OK) goto cleanup;
    old_cops_format = cops_format(resp);
    if (old_cops_format < 0) goto cleanup;
    if (old_cops_format != 2) {
        cops_changed = true;
        if (modem_cmd(m, "AT+COPS=3,2", resp, sizeof(resp), 3000) != ESP_OK) goto cleanup;
    }
    mms_gate_t gate = home_check(m, profile, resp, sizeof(resp));
    if (gate != MMS_GATE_OK) { if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED; goto cleanup; }
    if (old_cereg_mode != 1) {
        cereg_changed = true;
        if (modem_cmd(m, "AT+CEREG=1", resp, sizeof(resp), 3000) != ESP_OK) goto cleanup;
    }
    int req_len = mms_http_request(profile, url, request, sizeof(request));
    if (req_len <= 0) { result = ESP_ERR_NOT_SUPPORTED; goto cleanup; }

    if (!query_socket_idle(m, state, resp, sizeof(resp)) ||
        !query_call_idle(m, state, resp, sizeof(resp)) ||
        !query_mipcfg(m, "cid", &old_socket_cid, NULL, resp, sizeof(resp)) ||
        !query_mipcfg(m, "encoding", &old_tx_format, &old_rx_format, resp, sizeof(resp)) ||
        !query_mipcfg(m, "ssl", &old_ssl_enable, &old_ssl_id, resp, sizeof(resp))) goto cleanup;

    if (!read_context(m, old_type, sizeof(old_type), old_apn, sizeof(old_apn),
                      &old_context_found, resp, sizeof(resp))) goto cleanup;
    char cmd[256];
    int cn;
    if (old_context_found) {
        if (strcmp(old_apn, profile->apn) != 0 ||
            (strcmp(old_type, "IP") != 0 && strcmp(old_type, "IPV4V6") != 0)) {
            result = ESP_ERR_NOT_SUPPORTED;
            goto cleanup;
        }
    } else {
        cn = snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"IP\",\"%s\"", MMS_CID, profile->apn);
        if (cn <= 0 || (size_t)cn >= sizeof(cmd)) goto cleanup;
        context_changed = true;
        if (modem_cmd(m, cmd, resp, sizeof(resp), MMS_AT_TIMEOUT_MS) != ESP_OK) goto cleanup;
    }

    gate = home_check(m, profile, resp, sizeof(resp));
    if (gate != MMS_GATE_OK) { if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED; goto cleanup; }
    if (!query_call_idle(m, state, resp, sizeof(resp))) goto cleanup;
    gate = state_gate(m);
    if (gate != MMS_GATE_OK) { if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED; goto cleanup; }
    taskENTER_CRITICAL(&m->rx_mux);
    state->call_states[MMS_CID] = -1; /* Ignore a prior explicit inactive query row. */
    taskEXIT_CRITICAL(&m->rx_mux);
    call_started = true;
    if (modem_cmd(m, "AT+MIPCALL=1,5", resp, sizeof(resp), 12000) != ESP_OK) goto cleanup;
    int64_t call_deadline = esp_timer_get_time() + 20000000;
    int64_t next_gate_check = esp_timer_get_time() + 1000000;
    while (esp_timer_get_time() < call_deadline) {
        gate = state_gate(m);
        if (gate != MMS_GATE_OK) {
            if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED;
            goto cleanup;
        }
        taskENTER_CRITICAL(&m->rx_mux);
        int call_result = state->call_states[MMS_CID];
        taskEXIT_CRITICAL(&m->rx_mux);
        if (call_result >= 0) {
            if (call_result != 1) goto cleanup;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        if (esp_timer_get_time() >= next_gate_check) {
            gate = home_check(m, profile, resp, sizeof(resp));
            if (gate != MMS_GATE_OK) {
                if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED;
                goto cleanup;
            }
            next_gate_check = esp_timer_get_time() + 1000000;
        }
    }
    taskENTER_CRITICAL(&m->rx_mux);
    bool call_active = state->call_states[MMS_CID] == 1;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (!call_active) goto cleanup;

    gate = home_check(m, profile, resp, sizeof(resp));
    if (gate != MMS_GATE_OK) { if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED; goto cleanup; }
    if (!query_socket_idle(m, state, resp, sizeof(resp))) goto cleanup;
    gate = state_gate(m);
    if (gate != MMS_GATE_OK) { if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED; goto cleanup; }
    cfg_changed = true;
    if (modem_cmd(m, "AT+MIPCFG=\"cid\",5,5", resp, sizeof(resp), MMS_AT_TIMEOUT_MS) != ESP_OK)
        goto cleanup;
    if (modem_cmd(m, "AT+MIPCFG=\"encoding\",5,1,1", resp, sizeof(resp), MMS_AT_TIMEOUT_MS) != ESP_OK)
        goto cleanup;
    if (modem_cmd(m, "AT+MIPCFG=\"ssl\",5,0,0", resp, sizeof(resp), MMS_AT_TIMEOUT_MS) != ESP_OK)
        goto cleanup;
    gate = home_check(m, profile, resp, sizeof(resp));
    if (gate != MMS_GATE_OK) { if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED; goto cleanup; }
    cn = snprintf(cmd, sizeof(cmd), "AT+MIPOPEN=5,\"TCP\",\"%s\",%u,60,2,0", profile->proxy, profile->port);
    if (cn <= 0 || (size_t)cn >= sizeof(cmd)) goto cleanup;
    socket_opened = true;
    if (modem_cmd(m, cmd, resp, sizeof(resp), MMS_AT_TIMEOUT_MS) != ESP_OK) goto cleanup;
    int64_t connect_deadline = esp_timer_get_time() + (int64_t)MMS_CONNECT_TIMEOUT_MS * 1000;
    next_gate_check = esp_timer_get_time() + 1000000;
    while (esp_timer_get_time() < connect_deadline) {
        gate = state_gate(m);
        if (gate != MMS_GATE_OK) {
            if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED;
            goto cleanup;
        }
        taskENTER_CRITICAL(&m->rx_mux);
        int open_result = state->open_result;
        taskEXIT_CRITICAL(&m->rx_mux);
        if (open_result >= 0) {
            if (open_result != 0) goto cleanup;
            socket_connected = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        if (esp_timer_get_time() >= next_gate_check) {
            gate = home_check(m, profile, resp, sizeof(resp));
            if (gate != MMS_GATE_OK) {
                if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED;
                goto cleanup;
            }
            next_gate_check = esp_timer_get_time() + 1000000;
        }
    }
    if (!socket_connected) goto cleanup;

    gate = home_check(m, profile, resp, sizeof(resp));
    if (gate != MMS_GATE_OK) { if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED; goto cleanup; }
    if (!send_request(m, profile, (const uint8_t *)request, (size_t)req_len,
                      resp, sizeof(resp), state, &gate)) {
        if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    const size_t wire_cap = MMS_BODY_MAX + MMS_HTTP_OVERHEAD_MAX;
    /* A 2 MiB body does not belong in the internal heap, which also has to hold the
     * TLS/HTTP state of the Feishu side; prefer PSRAM and keep malloc as fallback. */
    wire = (uint8_t *)heap_caps_malloc(wire_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wire) wire = (uint8_t *)malloc(wire_cap);
    if (!wire) { result = ESP_ERR_NO_MEM; goto cleanup; }
    int64_t total_deadline = esp_timer_get_time() + (int64_t)MMS_TOTAL_TIMEOUT_MS * 1000;
    int64_t next_home_poll = esp_timer_get_time() + (int64_t)MMS_HOME_POLL_MS * 1000;
    for (;;) {
        taskENTER_CRITICAL(&m->rx_mux);
        uint32_t available = state->available;
        eof = state->disconnected;
        bool lost = state->data_lost;
        taskEXIT_CRITICAL(&m->rx_mux);
        if (lost) {
            gate = state_gate(m);
            if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED;
            goto cleanup;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_home_poll) {
            gate = home_check(m, profile, resp, sizeof(resp));
            if (gate != MMS_GATE_OK) {
                if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED;
                goto cleanup;
            }
            next_home_poll = esp_timer_get_time() + (int64_t)MMS_HOME_POLL_MS * 1000;
            if (!available && !eof) {
                unsigned unread = 0;
                if (!query_unread(m, &unread, resp, sizeof(resp))) goto cleanup;
                taskENTER_CRITICAL(&m->rx_mux);
                state->available = unread;
                taskEXIT_CRITICAL(&m->rx_mux);
                available = unread;
            }
        }

        if (available) {
            gate = state_gate(m);
            if (esp_timer_get_time() >= total_deadline || gate != MMS_GATE_OK) {
                if (gate == MMS_GATE_BLOCKED) result = ESP_ERR_NOT_SUPPORTED;
                goto cleanup;
            }
            unsigned ask = available > MMS_READ_CHUNK ? MMS_READ_CHUNK : available;
            cn = snprintf(cmd, sizeof(cmd), "AT+MIPRD=%d,%u", MMS_SOCKET, ask);
            if (cn <= 0 || (size_t)cn >= sizeof(cmd) ||
                modem_cmd(m, cmd, resp, sizeof(resp), MMS_AT_TIMEOUT_MS) != ESP_OK) goto cleanup;
            unsigned unread = 0, actual = 0;
            const char *hex = NULL;
            if (!parse_read_reply(resp, &unread, &actual, &hex)) {
                /* The cache can drain between its URC and this AT command. */
                if (!query_unread(m, &unread, resp, sizeof(resp))) goto cleanup;
                taskENTER_CRITICAL(&m->rx_mux);
                state->available = unread;
                taskEXIT_CRITICAL(&m->rx_mux);
                continue;
            }
            if (actual > ask || (actual && !decode_hex_append(wire, wire_cap, &wire_len, hex, actual)))
                goto cleanup;
            taskENTER_CRITICAL(&m->rx_mux);
            state->available = unread;
            taskEXIT_CRITICAL(&m->rx_mux);

            if (!actual) continue;

            int decode = mms_http_decode(wire, wire_len, false, &decoded_len);
            if (decode < 0) goto cleanup;
            if (decode == 1) { result_ok = true; break; }
            if (wire_len >= wire_cap) goto cleanup;
            continue;
        }

        int decode = mms_http_decode(wire, wire_len, eof, &decoded_len);
        if (decode < 0) goto cleanup;
        if (decode == 1) { result_ok = true; break; }
        if (esp_timer_get_time() >= total_deadline) goto cleanup;
        if (eof) goto cleanup;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

cleanup:
    if (socket_opened) {
        (void)modem_cmd(m, "AT+MIPCLOSE=5", resp, sizeof(resp), MMS_AT_TIMEOUT_MS);
    }
    if (call_started) {
        (void)modem_cmd(m, "AT+MIPCALL=0,5", resp, sizeof(resp), MMS_AT_TIMEOUT_MS);
    }
    if (cfg_changed && !restore_mipcfg(m, old_socket_cid, old_tx_format, old_rx_format,
                                       old_ssl_enable, old_ssl_id, resp, sizeof(resp)))
        result_ok = false;
    if (context_changed && !restore_context(m, old_type, old_apn, old_context_found, resp, sizeof(resp)))
        result_ok = false;
    if (cereg_changed) {
        char restore[40];
        snprintf(restore, sizeof(restore), "AT+CEREG=%d", old_cereg_mode);
        if (modem_cmd(m, restore, resp, sizeof(resp), 3000) != ESP_OK) result_ok = false;
    }
    if (cops_changed) {
        char restore[40];
        snprintf(restore, sizeof(restore), "AT+COPS=3,%d", old_cops_format);
        if (modem_cmd(m, restore, resp, sizeof(resp), 3000) != ESP_OK) result_ok = false;
    }
    taskENTER_CRITICAL(&m->rx_mux);
    state->active = false;
    taskEXIT_CRITICAL(&m->rx_mux);
    modem_set_observer(m, NULL, NULL);

    if (result_ok) {
        *body = wire;
        *len = decoded_len;
        xSemaphoreGive(m->mms_lock);
        return ESP_OK;
    }
    free(wire);
    xSemaphoreGive(m->mms_lock);
    if (result == ESP_ERR_NOT_SUPPORTED || result == ESP_ERR_NO_MEM) return result;
    ESP_LOGW(TAG, "cellular MMS retrieval failed");
    return ESP_FAIL;
}
