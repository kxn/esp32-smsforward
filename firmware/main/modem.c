#include "modem.h"

#include <string.h>

#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "modem";

static bool is_urc(const char *line)
{
    static const char *prefixes[] = { "RING", "+CLIP:", "+CMTI:", "+CMT:", "RDY",
                                      "POWER DOWN", "+CFUN: 0", "NO CARRIER" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(line, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_mip_urc(const char *line)
{
    static const char *prefixes[] = { "+MIPURC:", "+MIPOPEN:", "+MIPCLOSE:",
                                      "+MIPCALL:", "+MIPSEND:", "+MIPSTATE:", "+CEREG:" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (strncmp(line, prefixes[i], strlen(prefixes[i])) == 0) return true;
    return false;
}

static void handle_line(modem_t *m, const char *line)
{
    if (line[0] == '\0') {
        return;
    }

    bool normal_urc = is_urc(line);
    taskENTER_CRITICAL(&m->rx_mux);
    bool has_observer = m->observer_cb != NULL;
    taskEXIT_CRITICAL(&m->rx_mux);
    bool observed_mip = has_observer && is_mip_urc(line);
    if (normal_urc || observed_mip) {
        if (m->urc_cb) {
            m->urc_cb(m, line);
        }
        modem_observer_cb_t observer;
        void *observer_ctx;
        taskENTER_CRITICAL(&m->rx_mux);
        observer = m->observer_cb;
        observer_ctx = m->observer_ctx;
        taskEXIT_CRITICAL(&m->rx_mux);
        if (observer) observer(m, line, observer_ctx);
        if (normal_urc || strncmp(line, "+CEREG:", 7) != 0) return;
        /* Registration reports are also unsolicited URCs. During an AT+CEREG?
         * transaction preserve the line for the command response parser. */
        taskENTER_CRITICAL(&m->rx_mux);
        bool pending_cereg = m->pending && !m->desynced;
        taskEXIT_CRITICAL(&m->rx_mux);
        if (!pending_cereg) return;
    }

    bool give_done = false;
    bool terminal_ok = strcmp(line, "OK") == 0;
    bool terminal_error = modem_terminal_error_line(line);
    taskENTER_CRITICAL(&m->rx_mux);
    if (m->recovering) {
        if (modem_recovery_prefix(m->recovery_probe_kind, line)) {
            m->recovery_prefix_seen = true;
        } else if (m->recovery_prefix_seen && (terminal_ok || terminal_error)) {
            m->recovering = false;
            if (terminal_ok) m->desynced = false;
            give_done = true;
        }
        taskEXIT_CRITICAL(&m->rx_mux);
        if (give_done) xSemaphoreGive(m->done);
        return;
    }
    if (!m->pending || m->desynced) {
        taskEXIT_CRITICAL(&m->rx_mux);
        return;
    }
    size_t l = strlen(line);
    if (m->resp_len + l + 2 < sizeof(m->resp)) {
        memcpy(m->resp + m->resp_len, line, l);
        m->resp_len += l;
        m->resp[m->resp_len++] = '\n';
        m->resp[m->resp_len] = '\0';
    } else {
        m->resp_overflow = true;
    }
    if (terminal_ok) {
        m->got_ok = true;
        give_done = true;
    } else if (terminal_error) {
        m->got_error = true;
        give_done = true;
    }
    if (modem_cmgs_ack_line(line)) m->got_cmgs_ack = true;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (give_done) {
        xSemaphoreGive(m->done);
        xSemaphoreGive(m->prompt); /* wake prompt waiter on explicit ERROR */
    }
}

static void rx_char(modem_t *m, char c)
{
    taskENTER_CRITICAL(&m->rx_mux);
    bool had_prompt = m->ignore_prompt_space;
    m->ignore_prompt_space = false;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (had_prompt && c == ' ') return;
    if (c == '>') {
        bool prompt = false;
        taskENTER_CRITICAL(&m->rx_mux);
        if (m->line_len == 0 && !m->line_overflow &&
            modem_prompt_active(m->pending, m->waiting_prompt, m->desynced)) {
            m->got_prompt = true;
            m->waiting_prompt = false;
            m->ignore_prompt_space = true;
            prompt = true;
        }
        taskEXIT_CRITICAL(&m->rx_mux);
        if (prompt) {
            m->line_len = 0;
            xSemaphoreGive(m->prompt);
        } else if (m->line_len + 1 < sizeof(m->line)) {
            m->line[m->line_len++] = c;
        }
        return;
    }
    if (c == '\r') return;
    if (c == '\n') {
        if (m->line_overflow) {
            m->line_overflow = false;
            m->line_len = 0;
            return;
        }
        m->line[m->line_len] = '\0';
        handle_line(m, m->line);
        m->line_len = 0;
    } else if (m->line_len + 1 < sizeof(m->line)) {
        m->line[m->line_len++] = c;
    } else {
        m->line_len = 0;
        m->line_overflow = true;
    }
}

static void rx_task(void *arg)
{
    modem_t *m = (modem_t *)arg;
    uint8_t buf[256];
    uart_event_t event;

    while (1) {
        /* Wait outside the UART RX mutex so reset can flush a silent UART.
         * Every queued event is a wakeup: the driver can drop data events
         * when the event queue is full, so drain beyond the event's size. */
        if (xQueueReceive(m->uart_events, &event, portMAX_DELAY) == pdTRUE) {
            int n;
            do {
                n = uart_read_bytes(m->uart, buf, sizeof(buf), 0);
                /* Flush can re-enable RX interrupts before releasing the
                 * driver mutex. Retry a contended read on the same wakeup. */
                if (n < 0) vTaskDelay(1);
                for (int i = 0; i < n; i++) {
                    rx_char(m, (char)buf[i]);
                }
            } while (n != 0);
        }
    }
}

const char *modem_name(const modem_t *m)
{
    return m->pin.name;
}

esp_err_t modem_init(modem_t *m, int idx)
{
    memset(m, 0, sizeof(*m));
    m->idx = idx;
    m->pin = MODEM_PINS[idx];
    m->uart = m->pin.uart;
    m->lock = xSemaphoreCreateMutex();
    m->mms_lock = xSemaphoreCreateMutex();
    m->done = xSemaphoreCreateBinary();
    m->prompt = xSemaphoreCreateBinary();
    if (!m->lock || !m->mms_lock || !m->done || !m->prompt) {
        return ESP_ERR_NO_MEM;
    }
    m->rx_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        /* The default is APB and changes under DFS. The modems can send
         * URCs at any time, so their UART clock must not depend on a PM lock
         * held only during commands. XTAL is stable through DFS. */
        .source_clk = UART_SCLK_XTAL,
    };

    ESP_ERROR_CHECK(uart_driver_install(m->uart, 4096, 2048, 20, &m->uart_events, 0));
    ESP_ERROR_CHECK(uart_param_config(m->uart, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(m->uart, m->pin.tx, m->pin.rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << m->pin.rst,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(m->pin.rst, 0);

    xTaskCreate(rx_task, m->pin.name, 4096, m, 12, NULL);
    ESP_LOGI(TAG, "%s uart%d TX=%d RX=%d RST=%d", m->pin.name, m->uart,
             m->pin.tx, m->pin.rx, m->pin.rst);
    return ESP_OK;
}

static void response_reset_locked(modem_t *m, bool prompt_phase)
{
    taskENTER_CRITICAL(&m->rx_mux);
    m->resp_len = 0;
    m->resp[0] = '\0';
    m->resp_overflow = false;
    m->got_ok = false;
    m->got_error = false;
    m->got_cmgs_ack = false;
    m->got_prompt = false;
    m->ignore_prompt_space = false;
    m->pending = true;
    m->waiting_prompt = prompt_phase;
    taskEXIT_CRITICAL(&m->rx_mux);
    xSemaphoreTake(m->done, 0);
    xSemaphoreTake(m->prompt, 0);
}

/* Caller holds m->lock. A timeout invalidates transaction boundaries. Only a
 * non-mutating query with its own data prefix followed by OK can restore them. */
static bool recover_boundary_locked(modem_t *m)
{
    static const char *const probe_cmds[] = { NULL, "AT+CPIN?", "AT+CSCS?", "AT+CSQ" };
    unsigned kind = 0;
    for (unsigned candidate = 1; candidate <= 3; candidate++) {
        if (!(m->unsafe_probe_mask & (1u << (candidate - 1)))) {
            kind = candidate;
            break;
        }
    }
    if (!kind) return false;
    const char *probe = probe_cmds[kind];
    taskENTER_CRITICAL(&m->rx_mux);
    m->recovering = true;
    m->recovery_prefix_seen = false;
    m->recovery_probe_kind = (uint8_t)kind;
    m->resp_len = 0;
    m->resp[0] = '\0';
    m->got_ok = false;
    m->got_error = false;
    m->got_cmgs_ack = false;
    taskEXIT_CRITICAL(&m->rx_mux);
    xSemaphoreTake(m->done, 0);
    xSemaphoreTake(m->prompt, 0);
    char command[24];
    int command_len = snprintf(command, sizeof(command), "%s\r\n", probe);
    uart_write_bytes(m->uart, command, command_len);
    bool signaled = xSemaphoreTake(m->done, pdMS_TO_TICKS(3000)) == pdTRUE;
    taskENTER_CRITICAL(&m->rx_mux);
    bool recovered = signaled && !m->desynced && !m->recovering;
    if (!recovered) {
        m->recovering = false;
        m->desynced = true;
        m->unsafe_probe_mask |= (uint8_t)(1u << (kind - 1));
    } else {
        m->unsafe_probe_mask = 0;
    }
    taskEXIT_CRITICAL(&m->rx_mux);
    return recovered;
}

static void mark_timed_out_command(modem_t *m, const char *cmd)
{
    strlcpy(m->timed_out_cmd, cmd, sizeof(m->timed_out_cmd));
    if (strcmp(cmd, "AT+CPIN?") == 0) m->unsafe_probe_mask |= 1u << 0;
    else if (strcmp(cmd, "AT+CSCS?") == 0) m->unsafe_probe_mask |= 1u << 1;
    else if (strcmp(cmd, "AT+CSQ") == 0) m->unsafe_probe_mask |= 1u << 2;
}

static esp_err_t transaction_lock(modem_t *m, int timeout_ms)
{
    if (xSemaphoreTake(m->lock, pdMS_TO_TICKS(timeout_ms + 1000)) != pdTRUE)
        return MODEM_ERR_BUSY;
    taskENTER_CRITICAL(&m->rx_mux);
    bool desynced = m->desynced;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (desynced && !recover_boundary_locked(m)) {
        xSemaphoreGive(m->lock);
        return MODEM_ERR_DESYNC;
    }
    return ESP_OK;
}

static void finish_transaction(modem_t *m, char *out, size_t outlen, bool terminal)
{
    taskENTER_CRITICAL(&m->rx_mux);
    m->pending = false;
    m->waiting_prompt = false;
    if (!terminal) {
        m->desynced = true;
    }
    taskEXIT_CRITICAL(&m->rx_mux);
    if (out && outlen) snprintf(out, outlen, "%s", m->resp);
}

static esp_err_t transaction_result(modem_t *m, bool signaled, bool submit_started)
{
    taskENTER_CRITICAL(&m->rx_mux);
    bool ok = m->got_ok;
    bool error = m->got_error;
    bool overflow = m->resp_overflow;
    taskEXIT_CRITICAL(&m->rx_mux);
    return modem_transaction_result(signaled || ok || error, ok, overflow, submit_started);
}

static esp_err_t sms_submission_result(modem_t *m, bool signaled)
{
    taskENTER_CRITICAL(&m->rx_mux);
    bool ok = m->got_ok;
    bool error = m->got_error;
    bool cmgs_ack = m->got_cmgs_ack;
    taskEXIT_CRITICAL(&m->rx_mux);
    return modem_sms_result(signaled || ok || error, ok, error, cmgs_ack);
}

void modem_set_observer(modem_t *m, modem_observer_cb_t cb, void *ctx)
{
    if (!m) return;
    taskENTER_CRITICAL(&m->rx_mux);
    m->observer_cb = cb;
    m->observer_ctx = ctx;
    taskEXIT_CRITICAL(&m->rx_mux);
}

static esp_err_t modem_cmd_locked(modem_t *m, const char *cmd, char *out, size_t outlen, int timeout_ms)
{
    if (out && outlen) out[0] = '\0';
    if (!m || !cmd) return ESP_ERR_INVALID_ARG;
    char buf[320];
    if (strlen(cmd) + 3 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    response_reset_locked(m, false);

    snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    uart_write_bytes(m->uart, buf, strlen(buf));

    bool signaled = xSemaphoreTake(m->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    esp_err_t result = transaction_result(m, signaled, false);
    bool terminal = result != ESP_ERR_TIMEOUT;
    if (!terminal) mark_timed_out_command(m, cmd);
    taskENTER_CRITICAL(&m->rx_mux);
    m->ready = m->got_ok;
    taskEXIT_CRITICAL(&m->rx_mux);
    finish_transaction(m, out, outlen, terminal);
    return result;
}

esp_err_t modem_cmd(modem_t *m, const char *cmd, char *out, size_t outlen, int timeout_ms)
{
    if (out && outlen) out[0] = '\0';
    if (!m || !cmd) return ESP_ERR_INVALID_ARG;
    esp_err_t err = transaction_lock(m, timeout_ms);
    if (err != ESP_OK) return err;
    err = modem_cmd_locked(m, cmd, out, outlen, timeout_ms);
    xSemaphoreGive(m->lock);
    return err;
}

esp_err_t modem_sms_pdu(modem_t *m, const char *pdu_hex, int tpdu_len,
                        char *out, size_t outlen, int timeout_ms)
{
    if (out && outlen) out[0] = '\0';
    if (!m || !pdu_hex) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = transaction_lock(m, timeout_ms + 1000);
    if (lock_err != ESP_OK) return lock_err;
    response_reset_locked(m, true);

    char cmd[48];
    int cl = snprintf(cmd, sizeof(cmd), "AT+CMGS=%d\r\n", tpdu_len);
    uart_write_bytes(m->uart, cmd, cl);

    bool prompt_signal = xSemaphoreTake(m->prompt, pdMS_TO_TICKS(5000)) == pdTRUE;
    taskENTER_CRITICAL(&m->rx_mux);
    bool got_prompt = prompt_signal && m->got_prompt;
    bool got_error = m->got_error;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (!got_prompt) {
        if (!got_error) {
            const uint8_t esc = 0x1B;
            uart_write_bytes(m->uart, &esc, 1);
            bool abort_done = xSemaphoreTake(m->done, pdMS_TO_TICKS(1500)) == pdTRUE;
            esp_err_t abort_result = transaction_result(m, abort_done, false);
            bool clean_abort = abort_result == ESP_OK || abort_result == ESP_FAIL;
            if (!clean_abort) mark_timed_out_command(m, "AT+CMGS");
            finish_transaction(m, out, outlen, clean_abort);
            xSemaphoreGive(m->lock);
            return MODEM_ERR_PROMPT_TIMEOUT;
        }
        finish_transaction(m, out, outlen, true);
        xSemaphoreGive(m->lock);
        return ESP_FAIL;
    }

    uart_write_bytes(m->uart, pdu_hex, strlen(pdu_hex));
    const uint8_t ctrlz = 0x1A;
    uart_write_bytes(m->uart, &ctrlz, 1);

    bool signaled = xSemaphoreTake(m->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    esp_err_t result = sms_submission_result(m, signaled);
    if (result == MODEM_ERR_SUBMIT_UNKNOWN) mark_timed_out_command(m, "AT+CMGS");
    finish_transaction(m, out, outlen, result != MODEM_ERR_SUBMIT_UNKNOWN);
    xSemaphoreGive(m->lock);
    return result;
}

esp_err_t modem_sms_text(modem_t *m, const char *dest, const char *text,
                         char *out, size_t outlen, int timeout_ms)
{
    if (out && outlen) out[0] = '\0';
    if (!m || !dest || !text) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = transaction_lock(m, timeout_ms + 1000);
    if (lock_err != ESP_OK) return lock_err;
    response_reset_locked(m, true);

    char cmd[80];
    int cl = snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"\r\n", dest);
    uart_write_bytes(m->uart, cmd, cl);

    bool prompt_signal = xSemaphoreTake(m->prompt, pdMS_TO_TICKS(5000)) == pdTRUE;
    taskENTER_CRITICAL(&m->rx_mux);
    bool got_prompt = prompt_signal && m->got_prompt;
    bool got_error = m->got_error;
    taskEXIT_CRITICAL(&m->rx_mux);
    if (!got_prompt) {
        if (!got_error) {
            const uint8_t esc = 0x1B;
            uart_write_bytes(m->uart, &esc, 1);
            bool abort_done = xSemaphoreTake(m->done, pdMS_TO_TICKS(1500)) == pdTRUE;
            esp_err_t abort_result = transaction_result(m, abort_done, false);
            bool clean_abort = abort_result == ESP_OK || abort_result == ESP_FAIL;
            if (!clean_abort) mark_timed_out_command(m, "AT+CMGS");
            finish_transaction(m, out, outlen, clean_abort);
            xSemaphoreGive(m->lock);
            return MODEM_ERR_PROMPT_TIMEOUT;
        }
        finish_transaction(m, out, outlen, true);
        xSemaphoreGive(m->lock);
        return ESP_FAIL;
    }

    uart_write_bytes(m->uart, text, strlen(text));
    const uint8_t ctrlz = 0x1A;
    uart_write_bytes(m->uart, &ctrlz, 1);

    bool signaled = xSemaphoreTake(m->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    esp_err_t result = sms_submission_result(m, signaled);
    if (result == MODEM_ERR_SUBMIT_UNKNOWN) mark_timed_out_command(m, "AT+CMGS");
    finish_transaction(m, out, outlen, result != MODEM_ERR_SUBMIT_UNKNOWN);
    xSemaphoreGive(m->lock);
    return result;
}

void modem_reset_hw(modem_t *m)
{
    ESP_LOGW(TAG, "%s hardware reset", modem_name(m));
    if (xSemaphoreTake(m->lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "%s reset skipped: modem busy", modem_name(m));
        return;
    }
    gpio_set_level(m->pin.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(m->pin.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    uart_flush_input(m->uart);
    taskENTER_CRITICAL(&m->rx_mux);
    m->pending = false;
    m->waiting_prompt = false;
    m->got_prompt = false;
    m->ignore_prompt_space = false;
    m->got_ok = false;
    m->got_error = false;
    m->got_cmgs_ack = false;
    m->desynced = false;
    m->recovering = false;
    m->recovery_prefix_seen = false;
    m->recovery_probe_kind = 0;
    taskEXIT_CRITICAL(&m->rx_mux);
    m->unsafe_probe_mask = 0;
    m->timed_out_cmd[0] = '\0';
    xSemaphoreTake(m->done, 0);
    xSemaphoreTake(m->prompt, 0);
    xSemaphoreGive(m->lock);
}

bool modem_is_desynced(modem_t *m)
{
    if (!m) return true;
    taskENTER_CRITICAL(&m->rx_mux);
    bool value = m->desynced;
    taskEXIT_CRITICAL(&m->rx_mux);
    return value;
}
