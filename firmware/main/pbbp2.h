#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PBBP2_MAX_HEADERS 24

typedef struct {
    char *key;
    char *value;
} pbbp2_header_t;

typedef struct {
    uint64_t seq_id;
    uint64_t log_id;
    int32_t service;
    int32_t method;          /* 0 = control, 1 = data */
    pbbp2_header_t headers[PBBP2_MAX_HEADERS];
    int header_count;
    char payload_encoding[16];
    char payload_type[64];
    uint8_t *payload;
    size_t payload_len;
    char log_id_new[64];
} pbbp2_frame_t;

void pbbp2_frame_init(pbbp2_frame_t *f);
void pbbp2_frame_free(pbbp2_frame_t *f);
const char *pbbp2_header_get(const pbbp2_frame_t *f, const char *key);
int pbbp2_header_set(pbbp2_frame_t *f, const char *key, const char *value);

/* 编解码：返回字节数 / <0 失败 */
int pbbp2_encode(const pbbp2_frame_t *f, uint8_t *out, size_t outlen);
int pbbp2_decode(const uint8_t *buf, size_t len, pbbp2_frame_t *f);

/* 自测 */
int pbbp2_selftest(char *report, size_t report_len);
