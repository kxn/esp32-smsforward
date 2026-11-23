#define MODEM_TXN_HOST_TEST
#include "../firmware/main/modem.h"

int main(void)
{
    if (!modem_prompt_active(true, true, false)) return 1;
    if (modem_prompt_active(false, true, false) ||
        modem_prompt_active(true, false, false) ||
        modem_prompt_active(true, true, true)) return 2;
    if (!modem_recovery_prefix(1, "+CPIN: READY") ||
        !modem_recovery_prefix(2, "+CSCS: \"GSM\"") ||
        !modem_recovery_prefix(3, "+CSQ: 18,99") ||
        modem_recovery_prefix(1, "+CSCS: \"GSM\"") ||
        modem_recovery_prefix(2, "OK")) return 3;
    if (!modem_terminal_error_line("ERROR") ||
        !modem_terminal_error_line("+CMS ERROR: 500") ||
        modem_terminal_error_line("NO CARRIER")) return 9;
    if (!modem_cmgs_ack_line("+CMGS: 123") ||
        !modem_cmgs_ack_line("+CMGS:   0   ") ||
        modem_cmgs_ack_line("+CMGS:") || modem_cmgs_ack_line("+CMGS: x") ||
        modem_cmgs_ack_line("+CMGS: 4,5")) return 10;
    if (modem_sms_result(true, true, false, true) != ESP_OK ||
        modem_sms_result(true, true, false, false) != MODEM_ERR_SUBMIT_UNKNOWN ||
        modem_sms_result(false, false, false, false) != MODEM_ERR_SUBMIT_UNKNOWN ||
        modem_sms_result(true, false, true, true) != ESP_FAIL) return 11;
    if (modem_transaction_result(false, false, false, false) != ESP_ERR_TIMEOUT) return 4;
    if (modem_transaction_result(false, false, false, true) != MODEM_ERR_SUBMIT_UNKNOWN) return 5;
    if (modem_transaction_result(true, true, false, true) != ESP_OK) return 6;
    if (modem_transaction_result(true, false, false, false) != ESP_FAIL) return 7;
    if (modem_transaction_result(true, true, true, false) != MODEM_ERR_OVERFLOW) return 8;
    return 0;
}
