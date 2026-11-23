#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define PDU_TEXT_MAX 1024

typedef struct {
    char sender[40];      /* 解码后的发件号码，国际格式带 + */
    char text[PDU_TEXT_MAX]; /* UTF-8 */
    int dcs;
    bool has_udh;
    bool mms_notification; /* WAP Push destination port 2948 */
    bool binary_payload_hex; /* 8-bit multipart payload encoded as hex for safe string assembly */
    int concat_ref;
    int concat_ref_width; /* 8 or 16 bits; avoids collisions across UDH forms */
    int concat_total;
    int concat_seq;
    time_t scts;          /* UTC */
    int scts_tz_quarters; /* 时区（15 分钟为单位） */
} sms_deliver_t;

/* 解码 SMS-DELIVER。返回 0 成功，<0 失败 */
int pdu_decode_deliver(const uint8_t *pdu, size_t len, sms_deliver_t *out);

/* 编码 SMS-SUBMIT（UCS2）。sca 为短信中心号码（可为 NULL，表示用模组默认）。
 * 返回 TPDU 字节数（不含 SCA），<0 失败。*tpdu_offset 返回 TPDU 在 PDU 中的起点。 */
int pdu_encode_submit(const char *sca, const char *dest, const char *text_utf8,
                      int ref, int total, int seq,
                      uint8_t *out, size_t outlen, size_t *tpdu_offset);

size_t pdu_hex2bin(const char *hex, uint8_t *out, size_t max);
void pdu_bin2hex(const uint8_t *bin, size_t len, char *out);

/* 内置自测，返回失败项数 */
int pdu_selftest(char *report, size_t report_len);
