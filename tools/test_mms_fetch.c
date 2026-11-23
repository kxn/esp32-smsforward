/* Exercise the production MMS transport and modem AT parser with a fake UART. */
#define MODEM_HOST_FAKE
#include <stdio.h>
#define TAG modem_fake_tag
#include "../firmware/main/modem.c"
#undef TAG
#define TAG mms_fake_tag
#include "../firmware/main/mms_fetch.c"
#undef TAG
#include "../firmware/main/mms_policy.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct fake_sem { int token; };

static modem_t g_modem;
static int64_t g_now_us;
static uint8_t g_http[4096];
static size_t g_http_len, g_http_off;
static char g_request[1400];
static size_t g_request_len;
static int g_reads, g_mipcall_start, g_cgdcnt_set, g_context_deleted;
static int g_mipopen, g_mipclose, g_cereq_enable, g_cereq_restore;
static int g_mipcfg_restore;
static bool g_socket_busy, g_cid5_busy, g_cid5_explicit_inactive;
static bool g_inject_roam, g_inject_drop, g_inject_bad_cereg;
static bool g_roam_on_activation, g_cereg_override, g_cereg_unknown, g_cops_foreign, g_imsi_foreign;

void fake_critical_enter(portMUX_TYPE *mux) { (void)mux; }
void fake_critical_exit(portMUX_TYPE *mux) { (void)mux; }

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_sem *s = calloc(1, sizeof(*s));
    if (s) s->token = 1;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(struct fake_sem)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    (void)ticks;
    if (!s || !s->token) return pdFALSE;
    s->token = 0;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (!s) return pdFALSE;
    s->token = 1;
    return pdTRUE;
}
BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { g_now_us += (int64_t)ticks * 1000; }
int64_t esp_timer_get_time(void) { return g_now_us; }

static void feed(const char *text)
{
    while (*text) rx_char(&g_modem, *text++);
}

static int hval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void mmsrd_query(void)
{
    char line[64];
    snprintf(line, sizeof(line), "+MIPRD: 5,%u\r\nOK\r\n", (unsigned)(g_http_len - g_http_off));
    feed(line);
}

static void mmsrd_read(unsigned requested)
{
    unsigned n = (unsigned)(g_http_len - g_http_off);
    if (n > requested) n = requested;
    char prefix[80];
    g_reads++;
    if (g_inject_roam && g_reads == 2) feed("+CEREG: 1,5\r\n");
    if (g_inject_bad_cereg && g_reads == 2) feed("+CEREG: invalid\r\n");
    if (g_inject_drop && g_reads == 2) feed("+MIPURC: \"drop\",5,12\r\n");
    size_t remain = g_http_len - g_http_off - n;
    int p = snprintf(prefix, sizeof(prefix), "+MIPRD: 5,%u,%u,", (unsigned)remain, n);
    assert(p > 0);
    for (int i = 0; i < p; ++i) rx_char(&g_modem, prefix[i]);
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = g_http[g_http_off + i];
        rx_char(&g_modem, hex[b >> 4]);
        rx_char(&g_modem, hex[b & 15]);
    }
    g_http_off += n;
    feed("\r\nOK\r\n");
}

static void handle_command(const char *cmd)
{
    g_now_us += 20000;
    if (!strcmp(cmd, "AT+CIMI")) feed(g_imsi_foreign ? "310260123456789\r\nOK\r\n" : "460010123456789\r\nOK\r\n");
    else if (!strcmp(cmd, "AT+CEREG?")) feed(g_cereg_override ? "+CEREG: 0,5\r\nOK\r\n" :
                                                 g_cereg_unknown ? "+CEREG: 0,2\r\nOK\r\n" :
                                                                   "+CEREG: 0,1\r\nOK\r\n");
    else if (!strcmp(cmd, "AT+CEREG=1")) { g_cereq_enable++; feed("OK\r\n"); }
    else if (!strcmp(cmd, "AT+CEREG=0")) { g_cereq_restore++; feed("OK\r\n"); }
    else if (!strcmp(cmd, "AT+COPS?")) feed(g_cops_foreign ? "+COPS: 0,2,\"46003\",7\r\nOK\r\n" : "+COPS: 0,2,\"46001\",7\r\nOK\r\n");
    else if (!strcmp(cmd, "AT+MIPSTATE=5")) {
        feed(g_socket_busy ? "+MIPSTATE: 5,\"TCP\",\"10.0.0.1\",80,\"CONNECTED\"\r\nOK\r\n" :
                             "+MIPSTATE: 5,,, ,\"INITIAL\"\r\nOK\r\n");
    }
    else if (!strcmp(cmd, "AT+MIPCALL?")) {
        if (g_cid5_busy) feed("+MIPCALL: 1,1,\"10.0.0.2\",\"\"\r\n+MIPCALL: 5,1,\"10.0.0.3\",\"\"\r\nOK\r\n");
        else if (g_cid5_explicit_inactive) feed("+MIPCALL: 1,1,\"10.0.0.2\",\"\"\r\n+MIPCALL: 5,0\r\nOK\r\n");
        else feed("+MIPCALL: 1,1,\"10.0.0.2\",\"\"\r\nOK\r\n");
    }
    else if (!strcmp(cmd, "AT+MIPCFG=\"cid\",5")) feed("+MIPCFG: \"cid\",5,1\r\nOK\r\n");
    else if (!strcmp(cmd, "AT+MIPCFG=\"encoding\",5")) feed("+MIPCFG: \"encoding\",5,0,0\r\nOK\r\n");
    else if (!strcmp(cmd, "AT+MIPCFG=\"ssl\",5")) feed("+MIPCFG: \"ssl\",5,0,0\r\nOK\r\n");
    else if (!strcmp(cmd, "AT+CGDCONT?")) feed("+CGDCONT: 1,\"IPV4V6\",\"\",,,,,\r\nOK\r\n");
    else if (!strncmp(cmd, "AT+CGDCONT=5,", 13)) { g_cgdcnt_set++; feed("OK\r\n"); }
    else if (!strcmp(cmd, "AT+CGDCONT=5")) { g_context_deleted++; feed("OK\r\n"); }
    else if (!strcmp(cmd, "AT+MIPCALL=1,5")) {
        g_mipcall_start++;
        if (g_roam_on_activation) feed("+CEREG: 1,5\r\n");
        feed("OK\r\n+MIPCALL: 5,1,\"10.0.0.3\",\"\"\r\n");
    }
    else if (!strcmp(cmd, "AT+MIPCALL=0,5")) feed("OK\r\n+MIPCALL: 5,0\r\n");
    else if (!strcmp(cmd, "AT+MIPCFG=\"cid\",5,5") ||
             !strcmp(cmd, "AT+MIPCFG=\"encoding\",5,1,1") ||
             !strcmp(cmd, "AT+MIPCFG=\"ssl\",5,0,0") ||
             !strcmp(cmd, "AT+MIPCFG=\"cid\",5,1") ||
             !strcmp(cmd, "AT+MIPCFG=\"encoding\",5,0,0")) {
        if (!strcmp(cmd, "AT+MIPCFG=\"cid\",5,1") ||
            !strcmp(cmd, "AT+MIPCFG=\"encoding\",5,0,0")) g_mipcfg_restore++;
        feed("OK\r\n");
    }
    else if (!strncmp(cmd, "AT+MIPOPEN=5,", 13)) {
        g_mipopen++;
        feed("OK\r\n+MIPOPEN: 5,0\r\n");
    }
    else if (!strncmp(cmd, "AT+MIPCLOSE=5", 13)) { g_mipclose++; feed("OK\r\n+MIPCLOSE: 5\r\n"); }
    else if (!strncmp(cmd, "AT+MIPSEND=5,", 13)) {
        const char *p = strchr(cmd + 13, ',');
        assert(p);
        unsigned len = (unsigned)strtoul(cmd + 13, NULL, 10);
        p++;
        assert(*p++ == '\"');
        for (unsigned i = 0; i < len; ++i) {
            int hi = hval(p[i * 2]), lo = hval(p[i * 2 + 1]);
            assert(hi >= 0 && lo >= 0 && g_request_len < sizeof(g_request));
            g_request[g_request_len++] = (char)((hi << 4) | lo);
        }
        if (g_request_len >= 4 && strstr(g_request, "\r\n\r\n")) {
            char urc[100];
            snprintf(urc, sizeof(urc), "+MIPURC: \"rtcp\",5,%u,%u\r\n",
                     (unsigned)g_http_len, (unsigned)g_http_len);
            feed(urc);
        }
        char ack[80];
        snprintf(ack, sizeof(ack), "+MIPSEND: 5,%u\r\nOK\r\n", len);
        feed(ack);
    }
    else if (!strcmp(cmd, "AT+MIPRD=5")) mmsrd_query();
    else if (!strncmp(cmd, "AT+MIPRD=5,", 11)) mmsrd_read((unsigned)strtoul(cmd + 11, NULL, 10));
    else if (!strncmp(cmd, "AT+COPS=3,", 10)) feed("OK\r\n");
    else feed("OK\r\n");
}

int uart_write_bytes(uart_port_t uart, const void *data, size_t len)
{
    (void)uart;
    const char *bytes = data;
    if (len >= 2 && bytes[len - 2] == '\r' && bytes[len - 1] == '\n') {
        char cmd[3200];
        assert(len - 2 < sizeof(cmd));
        memcpy(cmd, bytes, len - 2);
        cmd[len - 2] = '\0';
        handle_command(cmd);
    }
    return (int)len;
}

int uart_read_bytes(uart_port_t uart, void *buf, uint32_t len, uint32_t ticks)
{ (void)uart; (void)buf; (void)len; (void)ticks; return 0; }
int uart_driver_install(uart_port_t uart, int rx, int tx, int events, void *queue, int flags)
{ (void)uart; (void)rx; (void)tx; (void)events; (void)queue; (void)flags; return 0; }
int uart_param_config(uart_port_t uart, const uart_config_t *cfg) { (void)uart; (void)cfg; return 0; }
int uart_set_pin(uart_port_t uart, int tx, int rx, int rts, int cts)
{ (void)uart; (void)tx; (void)rx; (void)rts; (void)cts; return 0; }
int uart_flush_input(uart_port_t uart) { (void)uart; return 0; }
int gpio_config(const gpio_config_t *cfg) { (void)cfg; return 0; }
int gpio_set_level(int pin, int level) { (void)pin; (void)level; return 0; }

static void reset_fake(void)
{
    memset(&g_modem, 0, sizeof(g_modem));
    g_modem.idx = 0;
    g_modem.uart = 0;
    g_modem.pin.name = "fake";
    g_modem.lock = xSemaphoreCreateMutex();
    g_modem.mms_lock = xSemaphoreCreateMutex();
    g_modem.done = xSemaphoreCreateBinary();
    g_modem.prompt = xSemaphoreCreateBinary();
    g_modem.rx_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    g_now_us = 0;
    g_request_len = 0;
    g_http_off = 0;
    g_reads = g_mipcall_start = g_cgdcnt_set = g_context_deleted = 0;
    g_mipopen = g_mipclose = g_cereq_enable = g_cereq_restore = 0;
    g_mipcfg_restore = 0;
    g_socket_busy = g_cid5_busy = g_cid5_explicit_inactive = false;
    g_inject_roam = g_inject_drop = g_inject_bad_cereg = false;
    g_roam_on_activation = g_cereg_override = g_cereg_unknown = g_cops_foreign = g_imsi_foreign = false;
}

static void prepare_http(void)
{
    static uint8_t body[2500];
    for (size_t i = 0; i < sizeof(body); ++i) body[i] = (uint8_t)('A' + i % 26);
    int n = snprintf((char *)g_http, sizeof(g_http),
                     "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                     (unsigned)sizeof(body));
    assert(n > 0 && (size_t)n + sizeof(body) <= sizeof(g_http));
    memcpy(g_http + n, body, sizeof(body));
    g_http_len = (size_t)n + sizeof(body);
}

int main(void)
{
    prepare_http();
    reset_fake();
    g_cid5_explicit_inactive = true;
    uint8_t *body = NULL;
    size_t len = 0;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_OK);
    assert(len == 2500 && body[0] == 'A' && body[2499] == 'A' + (2499 % 26));
    assert(g_reads == 3 && g_mipcall_start == 1 && g_mipopen == 1 && g_mipclose == 1);
    assert(g_cgdcnt_set == 1 && g_context_deleted == 1 && g_cereq_enable == 1 && g_cereq_restore == 1);
    assert(g_mipcfg_restore == 2);
    assert(strstr(g_request, "GET http://mmsc.myuni.com.cn/m/1 HTTP/1.1\r\n"));
    free(body);

    reset_fake();
    g_socket_busy = true;
    body = NULL; len = 0;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_FAIL);
    assert(g_mipcall_start == 0 && g_cgdcnt_set == 0 && g_mipopen == 0);

    reset_fake();
    g_cid5_busy = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_FAIL);
    assert(g_mipcall_start == 0 && g_cgdcnt_set == 0 && g_mipopen == 0);

    reset_fake();
    g_inject_roam = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_ERR_NOT_SUPPORTED);
    assert(!body && len == 0 && g_mipclose == 1 && g_cereq_restore == 1);

    reset_fake();
    g_inject_drop = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_FAIL);
    assert(!body && len == 0 && g_mipclose == 1);

    reset_fake();
    g_cereg_override = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_ERR_NOT_SUPPORTED);
    assert(g_mipcall_start == 0 && g_mipopen == 0 && g_request_len == 0);

    reset_fake();
    g_cereg_unknown = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_FAIL);
    assert(g_mipcall_start == 0 && g_mipopen == 0 && g_request_len == 0);

    reset_fake();
    g_imsi_foreign = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_ERR_NOT_SUPPORTED);
    assert(g_mipcall_start == 0 && g_mipopen == 0 && g_request_len == 0);

    reset_fake();
    g_cops_foreign = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_ERR_NOT_SUPPORTED);
    assert(g_mipcall_start == 0 && g_mipopen == 0 && g_request_len == 0);

    reset_fake();
    g_inject_bad_cereg = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_FAIL);
    assert(g_mipclose == 1 && g_request_len > 0);

    reset_fake();
    g_roam_on_activation = true;
    assert(mms_fetch(&g_modem, "http://mmsc.myuni.com.cn/m/1", &body, &len) == ESP_ERR_NOT_SUPPORTED);
    assert(g_mipcall_start == 1 && g_mipopen == 0 && g_request_len == 0);

    puts("MMS fake modem: cache reads, cleanup, occupied resources, roaming, and drop passed");
    return 0;
}
