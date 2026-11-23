#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_WS_MESSAGE_MAX 32768
#define FEISHU_PBBP_PARTS_MAX 16
#define FEISHU_PBBP_SLOTS 4

typedef struct {
    uint8_t *data;
    size_t total;
    size_t received;
} feishu_ws_assembler_t;

typedef struct {
    char id[128];
    int sum;
    bool got[FEISHU_PBBP_PARTS_MAX];
    uint8_t *part[FEISHU_PBBP_PARTS_MAX];
    size_t len[FEISHU_PBBP_PARTS_MAX];
    int64_t touched_ms;
} feishu_pbbp_slot_t;

typedef struct { feishu_pbbp_slot_t slots[FEISHU_PBBP_SLOTS]; } feishu_pbbp_assembler_t;

/* 0=partial, 1=complete (caller owns *out), -1=invalid input. */
int feishu_ws_assembler_feed(feishu_ws_assembler_t *a, size_t offset, size_t total,
                             const void *chunk, size_t chunk_len, bool fin,
                             uint8_t **out, size_t *out_len);
void feishu_ws_assembler_reset(feishu_ws_assembler_t *a);

/* 0=partial, 1=complete (caller owns *out), -1=invalid/overflow. */
int feishu_pbbp_assembler_feed(feishu_pbbp_assembler_t *a, const char *message_id,
                               int sum, int seq, const void *part, size_t part_len,
                               int64_t now_ms, uint8_t **out, size_t *out_len);
void feishu_pbbp_assembler_reset(feishu_pbbp_assembler_t *a);
