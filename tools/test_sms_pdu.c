#include <stdio.h>
#include <string.h>
#include "../firmware/main/pdu.h"

static bool test_originator_phone(void)
{
    uint8_t pdu[] = {
        0x00, 0x04, 0x0b, 0x81, 0x31, 0x08, 0x10, 0x83, 0x00, 0xf0,
        0x00, 0x00, 0x62, 0x11, 0x22, 0x32, 0x21, 0x03, 0x23, 0x00,
    };
    sms_deliver_t d;
    if (pdu_decode_deliver(pdu, sizeof(pdu), &d) != 0 ||
        !d.sender_is_phone || strcmp(d.sender, "13800138000") != 0) return false;

    /* The same bytes with alphanumeric TON cannot be an originating phone address. */
    pdu[3] = 0xd0;
    if (pdu_decode_deliver(pdu, sizeof(pdu), &d) != 0 || d.sender_is_phone) return false;

    /* A misplaced filler produces the same display string, but is malformed. */
    pdu[3] = 0x81;
    pdu[9] = 0x0f;
    return pdu_decode_deliver(pdu, sizeof(pdu), &d) == 0 &&
        !d.sender_is_phone && strcmp(d.sender, "13800138000") == 0;
}

int main(void)
{
    char report[4096] = {0};
    int failed = pdu_selftest(report, sizeof(report));
    fputs(report, stdout);
    if (!test_originator_phone()) {
        fputs("FAIL SMS originator phone encoding\n", stderr);
        ++failed;
    }
    return failed ? 1 : 0;
}
