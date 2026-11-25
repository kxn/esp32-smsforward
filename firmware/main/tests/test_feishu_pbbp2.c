#include "../pbbp2.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    char report[256] = {0};
    if (pbbp2_selftest(report, sizeof(report)) != 0) {
        fputs(report, stderr);
        return 1;
    }
    /* Malformed varint and truncated fixed-width protobuf fields must fail cleanly. */
    const uint8_t bad_varint[] = { 0x08, 0x80 };
    pbbp2_frame_t f;
    if (pbbp2_decode(bad_varint, sizeof(bad_varint), &f) == 0) return 2;
    const uint8_t bad_fixed32[] = { 0x0D, 0x01 };
    if (pbbp2_decode(bad_fixed32, sizeof(bad_fixed32), &f) == 0) return 3;

    /* A repeated payload field must replace the previous value rather than leak it. */
    const uint8_t two_payloads[] = {
        0x08, 0x01,                                    /* seq_id = 1 */
        0x42, 0x03, 'o', 'n', 'e',                      /* payload = "one" */
        0x42, 0x03, 't', 'w', 'o',                      /* payload = "two" */
    };
    if (pbbp2_decode(two_payloads, sizeof(two_payloads), &f) != 0) return 4;
    if (f.payload_len != 3 || memcmp(f.payload, "two", 3) != 0) return 5;
    pbbp2_frame_free(&f);

    /* Headers need both halves: a NULL key or value must be refused, and a refused
     * header must leave the frame encodable. */
    pbbp2_frame_init(&f);
    if (pbbp2_header_set(&f, NULL, "x") == 0) return 6;
    if (pbbp2_header_set(&f, "k", NULL) == 0) return 7;
    if (f.header_count != 0) return 8;
    if (pbbp2_header_set(&f, "type", "ping") != 0) return 9;
    uint8_t buf[64];
    if (pbbp2_encode(&f, buf, sizeof(buf)) <= 0) return 10;
    pbbp2_frame_free(&f);
    puts("PASS pbbp2 roundtrip and malformed-frame rejection");
    return 0;
}
