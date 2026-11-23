#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

/* 转发消息格式：发件人 -> 卡N(本机号) / 时间 / 空行 / 正文 */
int fmt_incoming(char *out, size_t outlen, int slot, const char *slot_name,
                 const char *local_num, const char *peer, bool sender_is_phone,
                 time_t ts, bool time_synced,
                 const char *text);

/* 记录我们发出的飞书消息，供"引用回复"定位 */
void map_note(const char *msg_id, int slot, const char *peer);
void map_note_record(const char *msg_id, int slot, const char *peer, const char *record_id);
bool map_note_persisted(const char *msg_id, int slot, const char *peer, const char *record_id);
bool cmd_current_iccid(int slot, char out[24]);
bool map_find_notice(const char *record_id, char *msg_id, size_t capacity);
bool map_note_child(const char *parent_id, const char *child_id);
bool map_lookup(const char *msg_id, int *slot, char *peer, size_t peerlen);
bool map_lookup_record(const char *msg_id, int *slot, char *peer, size_t peerlen,
                       char *record_id, size_t record_id_len);
bool map_lookup_sim(const char *msg_id, int *slot, char *peer, size_t peerlen,
                    char *record_id, size_t record_id_len,
                    char *iccid, size_t iccid_len);

/* 处理一条来自飞书的文本消息 */
void cmd_handle(const char *open_id, const char *chat_id, const char *text,
                const char *msg_id, const char *parent_id);

/* 自测 */
int cmd_selftest(char *report, size_t report_len);

void cmd_init(void *modems);
void cmd_set_slot(int slot, const char *name, const char *number);
/* Refresh this SIM's current number from CNUM/CPBR; never return a stale cache on error. */
esp_err_t cmd_refresh_slot_number(int slot, char *number, size_t number_len);
const char *cmd_slot_number(int slot);
const char *cmd_slot_name(int slot);
