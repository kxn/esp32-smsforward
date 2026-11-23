#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/main/sms_text.h"

int main(void)
{
    /* A surrogate pair at the 67-unit boundary means naive ceil(units/67) loses data. */
    char input[512];
    size_t n = 0;
    for (int i = 0; i < 66; ++i) input[n++] = 'a';
    input[n++] = (char)0xF0; input[n++] = (char)0x9F;
    input[n++] = (char)0x98; input[n++] = (char)0x80;
    for (int i = 0; i < 66; ++i) input[n++] = 'b';
    input[n] = '\0';
    assert(sms_text_ucs2_units(input) == 134);
    assert(sms_text_segment_count(input) == 3);

    char rebuilt[512] = {0}, chunk[280];
    const char *p = input;
    size_t out = 0;
    int count = 0;
    while (*p) {
        int units = sms_text_next_chunk(p, chunk, sizeof(chunk), 67);
        assert(units > 0);
        size_t bytes = strlen(chunk);
        memcpy(rebuilt + out, chunk, bytes + 1);
        out += bytes;
        p += bytes;
        count++;
    }
    assert(count == 3 && strcmp(rebuilt, input) == 0);
    assert(sms_text_segment_count("你好") == 1);
    assert(sms_text_segment_count("\xF0\x28\x8C\x28") == -1);
    char latin[12] = {'A', 0, (char)0xe9, 'z'};
    assert(sms_text_display_8bit(latin,4,sizeof(latin))==5 && !strcmp(latin,"A?\xc3\xa9z"));
    char utf8[] = "你好";
    assert(sms_text_display_8bit(utf8,strlen(utf8),sizeof(utf8))==6 && !strcmp(utf8,"你好"));
    puts("SMS text segmentation tests passed");
    return 0;
}
