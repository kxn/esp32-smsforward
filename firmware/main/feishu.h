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
/* Liveness of the long connection is judged by *application* freshness, so this
 * is the floor of that window: the app-level test never declares a session stale
 * sooner than this.  A half-open TCP connection is already covered by the
 * driver's own RFC6455 PING/PONG watchdog (10 s ping, 120 s pong timeout); what
 * this catches is the other failure mode - a session that keeps answering
 * transport pings while the Feishu application layer delivers nothing. */
#define FEISHU_WS_RX_STALE_MS (150 * 1000)

esp_err_t feishu_ws_start(void);
void feishu_ws_stop(void);
/* True only while the session is known to be alive.  Only events we actually
 * observe clear it: a clean server-side close (CLOSED/FINISH), a transport error
 * (ERROR) and DISCONNECTED.  Nothing in the driver notices a client task that is
 * wedged while the socket still looks connected, which is why callers that need
 * "is the push channel really delivering" must also use feishu_ws_alive(). */
bool feishu_ws_connected(void);
/* Connected *and* the Feishu application layer delivered something within the
 * freshness window: max(3 x ping interval, FEISHU_WS_RX_STALE_MS), capped at
 * 10 min (6 min with the default 120 s application ping).  A session that only
 * answers transport-level pings - exactly the field outage mode - reports false. */
bool feishu_ws_alive(void);
/* Consecutive failed REST sends (0 = the last attempt worked). */
uint32_t feishu_send_fail_streak(void);
/* Milliseconds since the last *application-level* signal (an application pong or
 * a delivered event).  Until the first one arrives the CONNECTED instant is the
 * reference, so a fresh session reports its own age.  -1 when not connected or
 * never connected. */
int64_t feishu_ws_rx_age_ms(void);
/* Mark the current session dead so the next feishu_ws_start() rebuilds it.
 * Returns false and logs when there was no live session to mark (it is safe to
 * call without one: the repair ladder may have consumed a cooldown already). */
bool feishu_ws_force_reconnect(void);
/* Age of the current session in milliseconds, -1 when not connected. */
int64_t feishu_ws_connected_age_ms(void);
bool feishu_ws_authenticated_for(const char *app_id, const char *app_secret);
int feishu_ws_reconnects(void);
/* 供 URC/任务调用：记录推送目标（最近一次给机器人发消息的人） */
void feishu_learn_peer(const char *open_id, const char *chat_id);

/* 学到新的推送目标时回调（用于持久化） */
typedef void (*feishu_peer_cb_t)(const char *open_id, const char *chat_id);
void feishu_set_peer_cb(feishu_peer_cb_t cb);
