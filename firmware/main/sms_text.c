#include "sms_text.h"

#include <stdint.h>
#include <string.h>

static int utf8_sequence(const uint8_t *p, int *length, int *units)
{
    uint32_t cp;
    int n;
    if (*p < 0x80) { cp = *p; n = 1; }
    else if (*p >= 0xC2 && *p <= 0xDF) { cp = *p & 0x1F; n = 2; }
    else if (*p >= 0xE0 && *p <= 0xEF) { cp = *p & 0x0F; n = 3; }
    else if (*p >= 0xF0 && *p <= 0xF4) { cp = *p & 0x07; n = 4; }
    else return -1;
    for (int i = 1; i < n; ++i) {
        if (!p[i] || (p[i] & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
        (n == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return -1;
    *length = n;
    *units = cp > 0xFFFF ? 2 : 1;
    return 0;
}

int sms_text_ucs2_units(const char *text)
{
    if (!text) return -1;
    const uint8_t *p = (const uint8_t *)text;
    int total = 0;
    while (*p) {
        int n, units;
        if (utf8_sequence(p, &n, &units) != 0) return -1;
        total += units;
        p += n;
    }
    return total;
}

int sms_text_next_chunk(const char *text, char *out, size_t out_size, int max_units)
{
    if (!text || !out || !out_size || max_units < 1) return -1;
    const uint8_t *p = (const uint8_t *)text;
    size_t used = 0;
    int units = 0;
    while (*p) {
        int n, cost;
        if (utf8_sequence(p, &n, &cost) != 0) return -1;
        if (units + cost > max_units) break;
        if (used + (size_t)n + 1 > out_size) return -1;
        memcpy(out + used, p, (size_t)n);
        used += (size_t)n;
        units += cost;
        p += n;
    }
    out[used] = '\0';
    return units;
}

int sms_text_display_8bit(char *s, size_t len, size_t cap)
{
    if (!s || len >= cap) return -1;
    for (size_t i = 0; i < len; ++i) if (!s[i]) s[i] = '?';
    s[len] = 0;
    if (sms_text_ucs2_units(s) >= 0) return (int)len;
    size_t used = len;
    for (size_t i = 0; i < len; ++i) if ((unsigned char)s[i] >= 0x80) ++used;
    if (used >= cap) return -1;
    s[used] = 0;
    size_t end = used;
    while (len) {
        unsigned char c = (unsigned char)s[--len];
        if (c < 0x80) s[--end] = (char)c;
        else {
            s[--end] = (char)(0x80 | (c & 63));
            s[--end] = (char)(0xc0 | (c >> 6));
        }
    }
    return (int)used;
}

int sms_text_segment_count(const char *text)
{
    int total = sms_text_ucs2_units(text);
    if (total < 0) return -1;
    if (!total) return 0;
    int cap = total <= 70 ? 70 : 67;
    int count = 0;
    const char *p = text;
    char chunk[280];
    while (*p) {
        int n = sms_text_next_chunk(p, chunk, sizeof(chunk), cap);
        if (n <= 0) return -1;
        p += strlen(chunk);
        count++;
    }
    return count;
}
