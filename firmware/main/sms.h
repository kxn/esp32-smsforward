#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "modem.h"

enum { SMS_SEND_ERR_MODEM=-4, SMS_SEND_ERR_PARTIAL=-5 };

esp_err_t sms_init(modem_t *modems);
/* Own the receive queue on the existing modem bootstrap stack after initialization. */
void sms_run(void);
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
/* 当前空闲扫描间隔（毫秒），供状态页显示退避状态 */
uint32_t sms_sweep_delay_ms(void);
/* 去重日志最近一次读写是否正常；false 表示可能重复转发已发过的短信 */
bool sms_journal_ok(void);

/* 收信活动的累计计数与 SIM 存储水位，用于回答"短信到底有没有收到"。
 * 计数从本次启动开始，读取时整体快照。 */
typedef struct {
    unsigned cmti;        /* 收到的 +CMTI 新短信指示条数 */
    unsigned reads;       /* 成功解析出的存储短信条数 */
    unsigned delivered;   /* 成功转发（含彩信通知）条数 */
    unsigned kept;        /* 转发失败、留在卡上等待重试的条数 */
    unsigned deletes;     /* 成功删除的 SIM 条目数 */
    unsigned scans;       /* 执行过的 AT+CPMS? 扫描次数 */
    int64_t last_cmti_ms;     /* 最近一次 +CMTI 的 esp_timer 毫秒，0 表示本次启动还没收到 */
    int64_t last_delivered_ms;/* 最近一次成功转发的 esp_timer 毫秒 */
    unsigned sim_used[2];     /* 最近一次扫描看到的 SIM 已用条目数 */
    unsigned sim_capacity[2]; /* 对应卡的最大条目数 */
} sms_stats_t;

void sms_get_stats(sms_stats_t *out);
/* 立即触发一次两卡的 SIM 扫描（不等退避计时器） */
void sms_sweep_now(void);
/* 是否有来电正在振铃/接听；维护性复位应避开。 */
bool sms_call_active(void);
