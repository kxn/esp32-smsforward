/* Compile and execute the production modem.c with fake UART/FreeRTOS edges. */
#define MODEM_HOST_FAKE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/main/modem.c"

struct fake_sem { int token; int mutex; };
static int g_suppress_csq;
static int g_cmgs_mode; /* 0 prompt/no final, 1 explicit prompt error, 2 no prompt */
static int g_sms_final_mode; /* 0 none, 1 success, 2 explicit error */
static int g_at_count, g_payload_count, g_ctrlz_count, g_esc_count, g_urc_count;
static int g_ignore_next_gt;

enum { CMGS_UNKNOWN, CMGS_PROMPT_ERROR, CMGS_NO_PROMPT };
enum { SMS_FINAL_NONE, SMS_FINAL_OK, SMS_FINAL_ERROR, SMS_FINAL_OK_ONLY };
static modem_t g_modem;

void fake_critical_enter(portMUX_TYPE *mux) { (void)mux; }
void fake_critical_exit(portMUX_TYPE *mux) { (void)mux; }

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_sem *s = calloc(1, sizeof(*s));
    if (s) { s->token = 1; s->mutex = 1; }
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

static void feed(modem_t *m, const char *text)
{
    while (*text) rx_char(m, *text++);
}

static void fake_command(modem_t *m, const char *cmd)
{
    g_at_count++;
    if (strcmp(cmd, "AT+CSQ") == 0) {
        if (g_suppress_csq) { g_suppress_csq--; return; }
        if (g_ignore_next_gt) { rx_char(m, '>'); feed(m, "\r\n"); g_ignore_next_gt = 0; }
        feed(m, "+CSQ: 17,99\r\nOK\r\n");
    } else if (strcmp(cmd, "AT+CPIN?") == 0) {
        feed(m, "+CPIN: READY\r\nOK\r\n");
    } else if (strcmp(cmd, "AT+CSCS?") == 0) {
        feed(m, "+CSCS: \"GSM\"\r\nOK\r\n");
    } else if (strcmp(cmd, "AT+CEREG?") == 0) {
        feed(m, "AT+CEREG?\r\nNO CARRIER\r\n+CEREG: 2,1,\"12AF\",\"00BC\",7\r\nOK\r\n");
    } else if (strncmp(cmd, "AT+CMGS", 7) == 0) {
        if (g_cmgs_mode == CMGS_PROMPT_ERROR) feed(m, "+X: a>b\r\nERROR\r\n");
        else if (g_cmgs_mode != CMGS_NO_PROMPT) feed(m, "\r\n> ");
    }
}

int uart_write_bytes(uart_port_t uart, const void *data, size_t len)
{
    (void)uart;
    const unsigned char *bytes = data;
    if (len && bytes[len - 1] == '\n') {
        char cmd[384];
        size_t n = len < sizeof(cmd) ? len : sizeof(cmd) - 1;
        memcpy(cmd, bytes, n);
        cmd[n] = '\0';
        if (n >= 2 && cmd[n - 2] == '\r') cmd[n - 2] = '\0';
        fake_command(&g_modem, cmd);
    } else if (len == 1 && bytes[0] == 0x1A) {
        g_ctrlz_count++;
        if (g_sms_final_mode == SMS_FINAL_OK) feed(&g_modem, "+CMGS: 31\r\nOK\r\n");
        else if (g_sms_final_mode == SMS_FINAL_ERROR) feed(&g_modem, "+CMS ERROR: 500\r\n");
        else if (g_sms_final_mode == SMS_FINAL_OK_ONLY) feed(&g_modem, "OK\r\n");
    } else if (len == 1 && bytes[0] == 0x1B) {
        g_esc_count++;
    } else {
        g_payload_count++;
    }
    return (int)len;
}

int uart_read_bytes(uart_port_t uart, void *buf, uint32_t len, uint32_t ticks)
{ (void)uart; (void)buf; (void)len; (void)ticks; return 0; }
int uart_driver_install(uart_port_t u,int r,int t,int e,void*q,int f)
{ (void)u;(void)r;(void)t;(void)e;(void)q;(void)f;return 0; }
int uart_param_config(uart_port_t u,const uart_config_t*c) { (void)u;(void)c;return 0; }
int uart_set_pin(uart_port_t u,int tx,int rx,int rts,int cts)
{ (void)u;(void)tx;(void)rx;(void)rts;(void)cts;return 0; }
int uart_flush_input(uart_port_t u) { (void)u; return 0; }
int gpio_config(const gpio_config_t *cfg) { (void)cfg; return 0; }
int gpio_set_level(int p,int l) { (void)p;(void)l;return 0; }
BaseType_t xTaskCreate(void (*task)(void *),const char*n,unsigned s,void*a,unsigned p,void*h)
{ (void)task;(void)n;(void)s;(void)a;(void)p;(void)h;return pdPASS; }
void vTaskDelay(TickType_t t) { (void)t; }

static void count_urc(modem_t *m, const char *line)
{ (void)m; if (strcmp(line, "NO CARRIER") == 0) g_urc_count++; }

static void init_fake_modem(void)
{
    memset(&g_modem, 0, sizeof(g_modem));
    g_modem.uart = 0;
    g_modem.pin.name = "fake";
    g_modem.lock = xSemaphoreCreateMutex();
    g_modem.done = xSemaphoreCreateBinary();
    g_modem.prompt = xSemaphoreCreateBinary();
    g_modem.rx_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    g_modem.urc_cb = count_urc;
}

int main(void)
{
    init_fake_modem();
    char resp[512] = "stale";

    /* A caller that cannot acquire the transaction lock sends nothing and
     * receives an initialized output buffer. */
    assert(xSemaphoreTake(g_modem.lock, 0) == pdTRUE);
    int sent_before = g_at_count;
    assert(modem_cmd(&g_modem, "AT", resp, sizeof(resp), 10) == MODEM_ERR_BUSY);
    assert(resp[0] == '\0' && g_at_count == sent_before);
    xSemaphoreGive(g_modem.lock);

    /* Echo and an unrelated call URC cannot terminate or pollute CEREG. */
    assert(modem_cmd(&g_modem, "AT+CEREG?", resp, sizeof(resp), 10) == ESP_OK);
    assert(strstr(resp, "+CEREG: 2,1") && !strstr(resp, "NO CARRIER"));
    assert(g_urc_count == 1);

    /* '>' outside CMGS prompt phase is ordinary response data, not a prompt. */
    g_ignore_next_gt = 1;
    assert(modem_cmd(&g_modem, "AT+CSQ", resp, sizeof(resp), 10) == ESP_OK);
    assert(g_modem.got_prompt == false && strstr(resp, "+CSQ: 17,99"));

    /* Timeout quarantines a late OK. A distinct CPIN prefix+OK probe restores
     * the boundary before the requested CEREG command is sent. */
    g_suppress_csq = 1;
    assert(modem_cmd(&g_modem, "AT+CSQ", resp, sizeof(resp), 10) == ESP_ERR_TIMEOUT);
    assert(modem_is_desynced(&g_modem));
    feed(&g_modem, "OK\r\n");
    assert(modem_is_desynced(&g_modem));
    assert(modem_cmd(&g_modem, "AT+CEREG?", resp, sizeof(resp), 10) == ESP_OK);
    assert(!modem_is_desynced(&g_modem));
    assert(strstr(resp, "+CEREG: 2,1") && !strstr(resp, "+CPIN:"));

    /* An embedded '>' in a response/URC-style line is not the prompt; the
     * following ERROR returns without writing message or Ctrl-Z. */
    g_cmgs_mode = CMGS_PROMPT_ERROR;
    int payload_before = g_payload_count, ctrlz_before = g_ctrlz_count;
    assert(modem_sms_pdu(&g_modem, "001122", 3, resp, sizeof(resp), 10) == ESP_FAIL);
    assert(g_payload_count == payload_before && g_ctrlz_count == ctrlz_before);

    /* No prompt sends ESC only; post-Ctrl-Z timeout is unknown and emits one
     * body and one Ctrl-Z, with no automatic resubmission. */
    g_cmgs_mode = CMGS_NO_PROMPT;
    int esc_before = g_esc_count;
    assert(modem_sms_text(&g_modem, "+15550000001", "body", resp, sizeof(resp), 10) == MODEM_ERR_PROMPT_TIMEOUT);
    assert(g_esc_count == esc_before + 1 && g_payload_count == payload_before && g_ctrlz_count == ctrlz_before);

    g_cmgs_mode = CMGS_UNKNOWN;
    g_sms_final_mode = SMS_FINAL_NONE;
    payload_before = g_payload_count; ctrlz_before = g_ctrlz_count;
    assert(modem_sms_pdu(&g_modem, "001122", 3, resp, sizeof(resp), 10) == MODEM_ERR_SUBMIT_UNKNOWN);
    assert(g_payload_count == payload_before + 1 && g_ctrlz_count == ctrlz_before + 1);
    assert(modem_is_desynced(&g_modem));

    /* A modem-confirmed send succeeds after exactly one submission; an
     * explicit CMS rejection is a known failure, also with one submission. */
    g_sms_final_mode = SMS_FINAL_OK;
    payload_before = g_payload_count; ctrlz_before = g_ctrlz_count;
    assert(modem_sms_pdu(&g_modem, "003344", 3, resp, sizeof(resp), 10) == ESP_OK);
    assert(g_payload_count == payload_before + 1 && g_ctrlz_count == ctrlz_before + 1);
    g_sms_final_mode = SMS_FINAL_OK_ONLY;
    payload_before = g_payload_count; ctrlz_before = g_ctrlz_count;
    assert(modem_sms_pdu(&g_modem, "005566", 3, resp, sizeof(resp), 10) == MODEM_ERR_SUBMIT_UNKNOWN);
    assert(g_payload_count == payload_before + 1 && g_ctrlz_count == ctrlz_before + 1);
    assert(modem_is_desynced(&g_modem));
    g_sms_final_mode = SMS_FINAL_ERROR;
    payload_before = g_payload_count; ctrlz_before = g_ctrlz_count;
    assert(modem_sms_text(&g_modem, "+15550000001", "body", resp, sizeof(resp), 10) == ESP_FAIL);
    assert(g_payload_count == payload_before + 1 && g_ctrlz_count == ctrlz_before + 1);
    puts("production modem transaction flow passed");
    return 0;
}
