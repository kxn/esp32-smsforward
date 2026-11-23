#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define FEISHU_MEDIA_PARTS_MAX 8
#define FEISHU_MEDIA_TOTAL_MAX_BYTES (2u * 1024u * 1024u)
/* 20 KiB raw leaves room for worst-case JSON escaping under Feishu's
 * documented 150 KiB text-message limit. Larger bodies are skipped. */
#define FEISHU_MEDIA_TEXT_MAX_BYTES (20u * 1024u)

typedef struct {
    const char *mime_type;
    const char *charset;
    const char *filename;
    const uint8_t *data;
    size_t size;
} feishu_media_part_t;

typedef struct {
    size_t sent_count;
    size_t skipped_count;
    size_t part_indexes[FEISHU_MEDIA_PARTS_MAX];
    char message_ids[FEISHU_MEDIA_PARTS_MAX][64];
} feishu_media_result_t;

typedef struct {
    char app_id[64];
    char app_secret[128];
    char open_id[64];
    char chat_id[64];
} feishu_cfg_t;

/* 收到用户消息时的回调 */
typedef void (*feishu_cmd_cb_t)(const char *open_id, const char *chat_id,
                                const char *text, const char *msg_id,
                                const char *parent_id);

esp_err_t feishu_init(const feishu_cfg_t *cfg);
void feishu_set_cmd_cb(feishu_cmd_cb_t cb);
bool feishu_configured(void);

/* 发送文本，成功时把 message_id 写入 out */
esp_err_t feishu_send_text(const char *text, char *msg_id, size_t msg_id_len);
/* Send with a stable idempotency UUID for retry-safe delivery. */
esp_err_t feishu_send_text_uuid(const char *text, const char *uuid,
                                char *msg_id, size_t msg_id_len);
/* Reply media to an existing Feishu message, preserving its original private chat.
 * Input data is borrowed for the duration of this call. Unsupported parts are skipped.
 */
esp_err_t feishu_send_media_parts(const char *parent_id,
                                  const feishu_media_part_t *parts, size_t count,
                                  const char *stable_id_prefix,
                                  feishu_media_result_t *result);
/* 回复某条消息 */
esp_err_t feishu_reply_text(const char *parent_id, const char *text,
                            char *msg_id, size_t msg_id_len);

/* 长连接 */
esp_err_t feishu_ws_start(void);
void feishu_ws_stop(void);
bool feishu_ws_connected(void);
int feishu_ws_reconnects(void);
/* 供 URC/任务调用：记录推送目标（最近一次给机器人发消息的人） */
void feishu_learn_peer(const char *open_id, const char *chat_id);

/* 学到新的推送目标时回调（用于持久化） */
typedef void (*feishu_peer_cb_t)(const char *open_id, const char *chat_id);
void feishu_set_peer_cb(feishu_peer_cb_t cb);
