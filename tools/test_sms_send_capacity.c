#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/main/pdu.h"
#include "../firmware/main/sms_text.h"

int main(void)
{
    /* Eight concatenated segments at the full 67 UCS2-unit payload limit. */
    char text[67 * 8 + 1];
    memset(text, 'A', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    assert(sms_text_ucs2_units(text) == 536);
    assert(sms_text_segment_count(text) == 8);

    uint8_t pdu[700];
    char hex[1600], chunk[280];
    const char *at = text;
    for (int seq = 1; seq <= 8; seq++) {
        int units = sms_text_next_chunk(at, chunk, sizeof(chunk), 67);
        assert(units == 67);
        size_t off = 0;
        int tpdu = pdu_encode_submit(NULL, "+15550000001", chunk, 0x5A, 8, seq,
                                     pdu, sizeof(pdu), &off);
        assert(tpdu > 0 && (size_t)tpdu + off <= sizeof(pdu));
        pdu_bin2hex(pdu, (size_t)tpdu + off, hex);
        assert(strlen(hex) == 2 * ((size_t)tpdu + off));
        at += strlen(chunk);
    }
    assert(*at == '\0');
    puts("8x maximum SMS segment encode capacity passed");
    return 0;
}
