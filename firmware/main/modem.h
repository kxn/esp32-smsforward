#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#ifdef MODEM_TXN_HOST_TEST
#include <stdint.h>
typedef int32_t esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_TIMEOUT 0x107
#elif defined(MODEM_HOST_FAKE)
#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
typedef struct { int tx, rx, rst; const char *name; uart_port_t uart; } modem_pin_t;
#define MODEM_COUNT 2
static const modem_pin_t MODEM_PINS[MODEM_COUNT] = {{1,2,3,"fake0",0},{4,5,6,"fake1",1}};
#else
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "board.h"
#endif
#if !defined(MODEM_TXN_HOST_TEST)
#include "freertos/queue.h"
#endif

/* BUSY means no AT bytes were sent. DESYNC means the safe boundary probe
 * failed, so no command was sent. SUBMIT_UNKNOWN means Ctrl-Z was sent but no
 * final result arrived; callers must not automatically retry that message. */
#define MODEM_ERR_BUSY          ((esp_err_t)0x7101)
#define MODEM_ERR_DESYNC        ((esp_err_t)0x7102)
#define MODEM_ERR_PROMPT_TIMEOUT ((esp_err_t)0x7103)
#define MODEM_ERR_SUBMIT_UNKNOWN ((esp_err_t)0x7104)
#define MODEM_ERR_OVERFLOW      ((esp_err_t)0x7105)

static inline bool modem_prompt_active(bool pending, bool waiting_prompt, bool desynced)
{
    return pending && waiting_prompt && !desynced;
}

static inline bool modem_recovery_prefix(unsigned probe_kind, const char *line)
{
    const char *prefix = probe_kind == 1 ? "+CPIN:" :
                         probe_kind == 2 ? "+CSCS:" :
                         probe_kind == 3 ? "+CSQ:" : "";
    if (!prefix[0]) return false;
    return line && strncmp(line, prefix, strlen(prefix)) == 0;
}

static inline bool modem_terminal_error_line(const char *line)
{
    return line && (strncmp(line, "ERROR", 5) == 0 ||
                    strncmp(line, "+CME ERROR", 10) == 0 ||
                    strncmp(line, "+CMS ERROR", 10) == 0);
}

static inline bool modem_cmgs_ack_line(const char *line)
{
    if (!line || strncmp(line, "+CMGS:", 6) != 0) return false;
    const char *p = line + 6;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return false;
    do { p++; } while (*p >= '0' && *p <= '9');
    while (*p == ' ' || *p == '\t') p++;
    return *p == '\0';
}

static inline esp_err_t modem_sms_result(bool terminal, bool got_ok, bool got_error,
                                         bool got_cmgs_ack)
{
    if (!terminal) return MODEM_ERR_SUBMIT_UNKNOWN;
    if (got_error) return ESP_FAIL;
    if (got_ok && got_cmgs_ack) return ESP_OK;
    return MODEM_ERR_SUBMIT_UNKNOWN;
}

static inline esp_err_t modem_transaction_result(bool terminal, bool got_ok,
                                                  bool overflow, bool submit_started)
{
    if (!terminal) return submit_started ? MODEM_ERR_SUBMIT_UNKNOWN : ESP_ERR_TIMEOUT;
    if (overflow) return MODEM_ERR_OVERFLOW;
    return got_ok ? ESP_OK : ESP_FAIL;
}

#if !defined(MODEM_TXN_HOST_TEST)
typedef struct modem_s modem_t;

typedef void (*modem_urc_cb_t)(modem_t *m, const char *line);
typedef void (*modem_observer_cb_t)(modem_t *m, const char *line, void *ctx);

struct modem_s {
    int idx;
    modem_pin_t pin;
    uart_port_t uart;
    QueueHandle_t uart_events;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t mms_lock;
    SemaphoreHandle_t done;
    SemaphoreHandle_t prompt;
    portMUX_TYPE rx_mux;
    bool pending;
    bool waiting_prompt;
    bool got_prompt;
    bool ignore_prompt_space;
    bool got_ok;
    bool got_error;
    bool got_cmgs_ack;
    bool resp_overflow;
    bool desynced;
    bool recovering;
    bool recovery_prefix_seen;
    uint8_t recovery_probe_kind;
    uint8_t unsafe_probe_mask;
    bool line_overflow;
    char resp[8192];
    size_t resp_len;
    char line[2304];
    size_t line_len;
    char sca[24];          /* 短信中心号码，初始化时从 AT+CSCA? 读取 */
    char timed_out_cmd[32];
    modem_urc_cb_t urc_cb;
    modem_observer_cb_t observer_cb;
    void *observer_ctx;
    bool ready;
};

esp_err_t modem_init(modem_t *m, int idx);
esp_err_t modem_cmd(modem_t *m, const char *cmd, char *out, size_t outlen, int timeout_ms);
void modem_set_observer(modem_t *m, modem_observer_cb_t cb, void *ctx);
/* 发短信：先 AT+CMGS=<len> 等 '>' 提示符，再送 PDU + Ctrl-Z */
esp_err_t modem_sms_pdu(modem_t *m, const char *pdu_hex, int tpdu_len,
                        char *out, size_t outlen, int timeout_ms);
/* 文本模式发送（诊断用）：AT+CMGS="num" -> 文本 -> Ctrl-Z */
esp_err_t modem_sms_text(modem_t *m, const char *dest, const char *text,
                         char *out, size_t outlen, int timeout_ms);
void modem_reset_hw(modem_t *m);
bool modem_is_desynced(modem_t *m);
const char *modem_name(const modem_t *m);
#endif
