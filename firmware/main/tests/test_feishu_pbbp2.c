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
    puts("PASS pbbp2 roundtrip and malformed-frame rejection");
    return 0;
}
