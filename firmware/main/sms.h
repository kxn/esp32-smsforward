#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "modem.h"

enum { SMS_SEND_ERR_MODEM=-4, SMS_SEND_ERR_PARTIAL=-5 };

esp_err_t sms_init(modem_t *modems);
/* Reapply per-modem SMS storage, PDU mode, inbound URCs, and call reporting. */
esp_err_t sms_modem_configure(int slot);
bool sms_modem_is_configured(int slot);
/* 发短信（自动分段）。返回 0 成功，负数为错误码，err 里放原因 */
int sms_send(modem_t *m, const char *dest, const char *text, char *err, size_t errlen);
/* 主动读取一条存储位置的短信（调试用） */
int sms_read_index(modem_t *m, int idx);
/* 处理 URC（由 modem 层回调） */
void sms_urc(modem_t *m, const char *line);
/* 重试转发此前保留的短信（网络恢复后调用） */
void sms_retry_pending(void);
bool sms_clear_persistent_dedup(void);
