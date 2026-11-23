#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "feishu.h"
#include "feishu_gbk_table.h"

typedef enum {
    FEISHU_MEDIA_UNSUPPORTED = 0,
    FEISHU_MEDIA_TEXT,
    FEISHU_MEDIA_IMAGE,
    FEISHU_MEDIA_PDF,
} feishu_media_kind_t;

static inline bool feishu_media_mime_equal(const char *mime, const char *expected)
{
    if (!mime || !expected) return false;
    while (*mime == ' ' || *mime == '\t') mime++;
    size_t n = 0;
    while (mime[n] && mime[n] != ';' && mime[n] != ' ' && mime[n] != '\t') n++;
    return strlen(expected) == n && strncasecmp(mime, expected, n) == 0;
}

static inline feishu_media_kind_t feishu_media_classify(const char *mime)
{
    if (feishu_media_mime_equal(mime, "text/plain")) return FEISHU_MEDIA_TEXT;
    if (feishu_media_mime_equal(mime, "application/pdf")) return FEISHU_MEDIA_PDF;
    static const char *const image_types[] = {
        "image/jpeg", "image/jpg", "image/png", "image/webp", "image/gif",
        "image/bmp", "image/x-icon", "image/vnd.microsoft.icon", "image/tiff",
        "image/heic"
    };
    for (size_t i = 0; i < sizeof(image_types) / sizeof(image_types[0]); ++i)
        if (feishu_media_mime_equal(mime, image_types[i])) return FEISHU_MEDIA_IMAGE;
    return FEISHU_MEDIA_UNSUPPORTED;
}

/* Return only a constant MIME value suitable for an HTTP header. Never reuse
 * an untrusted MIME string (which may include parameters or control bytes). */
static inline const char *feishu_media_canonical_mime(const char *mime)
{
    if (feishu_media_mime_equal(mime, "application/pdf")) return "application/pdf";
    if (feishu_media_mime_equal(mime, "image/jpeg") || feishu_media_mime_equal(mime, "image/jpg")) return "image/jpeg";
    if (feishu_media_mime_equal(mime, "image/png")) return "image/png";
    if (feishu_media_mime_equal(mime, "image/webp")) return "image/webp";
    if (feishu_media_mime_equal(mime, "image/gif")) return "image/gif";
    if (feishu_media_mime_equal(mime, "image/bmp")) return "image/bmp";
    if (feishu_media_mime_equal(mime, "image/x-icon") || feishu_media_mime_equal(mime, "image/vnd.microsoft.icon")) return "image/x-icon";
    if (feishu_media_mime_equal(mime, "image/tiff")) return "image/tiff";
    if (feishu_media_mime_equal(mime, "image/heic")) return "image/heic";
    return NULL;
}

static inline const char *feishu_media_file_type(const char *mime)
{
    if (feishu_media_mime_equal(mime, "application/pdf")) return "pdf";
    return NULL;
}

static inline const char *feishu_media_image_extension(const char *mime)
{
    if (feishu_media_mime_equal(mime, "image/jpeg") || feishu_media_mime_equal(mime, "image/jpg")) return "jpg";
    if (feishu_media_mime_equal(mime, "image/png")) return "png";
    if (feishu_media_mime_equal(mime, "image/webp")) return "webp";
    if (feishu_media_mime_equal(mime, "image/gif")) return "gif";
    if (feishu_media_mime_equal(mime, "image/bmp")) return "bmp";
    if (feishu_media_mime_equal(mime, "image/x-icon") || feishu_media_mime_equal(mime, "image/vnd.microsoft.icon")) return "ico";
    if (feishu_media_mime_equal(mime, "image/tiff")) return "tiff";
    if (feishu_media_mime_equal(mime, "image/heic")) return "heic";
    return "bin";
}

static inline bool feishu_media_safe_filename(const char *raw, const char *fallback_ext,
                                              char *out, size_t cap)
{
    if (!out || cap < 2) return false;
    const char *base = raw && raw[0] ? raw : "attachment";
    const char *slash = strrchr(base, '/');
    const char *backslash = strrchr(base, '\\');
    if (slash && slash + 1 > base) base = slash + 1;
    if (backslash && backslash + 1 > base) base = backslash + 1;
    size_t n = 0;
    bool previous_dot = false;
    for (; *base && n + 1 < cap && n < 48; ++base) {
        unsigned char ch = (unsigned char)*base;
        char c = (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') ? (char)ch : '_';
        if (c == '.' && (n == 0 || previous_dot)) c = '_';
        out[n++] = c;
        previous_dot = c == '.';
    }
    while (n && out[n - 1] == '.') n--;
    if (!n) {
        int wrote = snprintf(out, cap, "attachment.%s", fallback_ext ? fallback_ext : "bin");
        return wrote > 0 && (size_t)wrote < cap;
    }
    out[n] = '\0';
    if (!strchr(out, '.') && fallback_ext && fallback_ext[0]) {
        int wrote = snprintf(out + n, cap - n, ".%s", fallback_ext);
        if (wrote < 0 || (size_t)wrote >= cap - n) return false;
    }
    return true;
}

/* Keep only a sanitized basename and force the extension from the allowlisted
 * MIME type so a hostile MMS filename cannot make an image look executable. */
static inline bool feishu_media_trusted_filename(const char *raw, const char *extension,
                                                 char *out, size_t cap)
{
    char sanitized[64];
    if (!out || cap < 2 || !extension || !extension[0] ||
        !feishu_media_safe_filename(raw, NULL, sanitized, sizeof(sanitized))) return false;
    char *dot = strrchr(sanitized, '.');
    if (dot) *dot = '\0';
    if (!sanitized[0]) memcpy(sanitized, "attachment", sizeof("attachment"));
    int n = snprintf(out, cap, "%s.%s", sanitized, extension);
    return n > 0 && (size_t)n < cap;
}

static inline bool feishu_media_payload_matches(const char *mime, const uint8_t *data, size_t len)
{
    if (!data) return false;
    if (feishu_media_mime_equal(mime, "image/jpeg"))
        return len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    if (feishu_media_mime_equal(mime, "image/png")) {
        static const uint8_t sig[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        return len >= sizeof(sig) && memcmp(data, sig, sizeof(sig)) == 0;
    }
    if (feishu_media_mime_equal(mime, "image/gif"))
        return len >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6));
    if (feishu_media_mime_equal(mime, "image/webp"))
        return len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4);
    if (feishu_media_mime_equal(mime, "image/bmp")) return len >= 2 && data[0] == 'B' && data[1] == 'M';
    if (feishu_media_mime_equal(mime, "image/x-icon") || feishu_media_mime_equal(mime, "image/vnd.microsoft.icon"))
        return len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0;
    if (feishu_media_mime_equal(mime, "image/tiff"))
        return len >= 4 && ((!memcmp(data, "II*\0", 4)) || (!memcmp(data, "MM\0*", 4)));
    if (feishu_media_mime_equal(mime, "image/heic"))
        return len >= 12 && !memcmp(data + 4, "ftyp", 4) &&
               (!memcmp(data + 8, "heic", 4) || !memcmp(data + 8, "heix", 4) ||
                !memcmp(data + 8, "hevc", 4) || !memcmp(data + 8, "mif1", 4));
    if (feishu_media_mime_equal(mime, "application/pdf")) return len >= 5 && !memcmp(data, "%PDF-", 5);
    return false;
}

static inline bool feishu_media_valid_utf8(const uint8_t *s, size_t n)
{
    if (!s && n) return false;
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i];
        if (c == 0) return false;
        if (c < 0x80) { i++; continue; }
        size_t extra;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { extra = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; }
        else return false;
        if (extra > n - i - 1) return false;
        for (size_t j = 1; j <= extra; ++j) {
            if ((s[i + j] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + j] & 0x3F);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return false;
        i += extra + 1;
    }
    return true;
}

static inline bool feishu_media_append_utf8(uint32_t cp, char *out, size_t cap, size_t *used)
{
    uint8_t bytes[4]; size_t n;
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (cp < 0x80) { bytes[0] = (uint8_t)cp; n = 1; }
    else if (cp < 0x800) { bytes[0] = 0xC0 | (cp >> 6); bytes[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { bytes[0] = 0xE0 | (cp >> 12); bytes[1] = 0x80 | ((cp >> 6) & 0x3F); bytes[2] = 0x80 | (cp & 0x3F); n = 3; }
    else { bytes[0] = 0xF0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 0x3F); bytes[2] = 0x80 | ((cp >> 6) & 0x3F); bytes[3] = 0x80 | (cp & 0x3F); n = 4; }
    if (*used > cap || n > cap - *used) return false;
    memcpy(out + *used, bytes, n);
    *used += n;
    return true;
}

static inline bool feishu_media_decode_text(const char *charset, const uint8_t *data,
                                            size_t len, char *out, size_t cap,
                                            size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!out || !cap || (!data && len) || len > FEISHU_MEDIA_TOTAL_MAX_BYTES) return false;
    size_t used = 0;
    bool utf8 = !charset || !charset[0] || !strcasecmp(charset, "UTF-8") || !strcasecmp(charset, "UTF8");
    bool ascii = charset && (!strcasecmp(charset, "US-ASCII") || !strcasecmp(charset, "ASCII"));
    bool latin1 = charset && (!strcasecmp(charset, "ISO-8859-1") || !strcasecmp(charset, "LATIN1"));
    bool be = charset && (!strcasecmp(charset, "UTF-16BE") || !strcasecmp(charset, "UTF16BE"));
    bool le = charset && (!strcasecmp(charset, "UTF-16LE") || !strcasecmp(charset, "UTF16LE"));
    bool gbk = charset && (!strcasecmp(charset, "GBK") || !strcasecmp(charset, "CP936") || !strcasecmp(charset, "GB2312"));
    bool gb2312 = charset && !strcasecmp(charset, "GB2312");
    if (len >= 2 && data[0] == 0xFE && data[1] == 0xFF) { be = true; le = false; utf8 = ascii = latin1 = gbk = false; data += 2; len -= 2; }
    else if (len >= 2 && data[0] == 0xFF && data[1] == 0xFE) { le = true; be = false; utf8 = ascii = latin1 = gbk = false; data += 2; len -= 2; }
    if (utf8) {
        if (!feishu_media_valid_utf8(data, len) || len >= cap) return false;
        memcpy(out, data, len); used = len;
    } else if (ascii) {
        if (len >= cap) return false;
        for (size_t i = 0; i < len; ++i) { if (data[i] > 0x7F || !data[i]) return false; out[used++] = (char)data[i]; }
    } else if (latin1) {
        for (size_t i = 0; i < len; ++i)
            if (!feishu_media_append_utf8(data[i], out, cap - 1, &used)) return false;
    } else if (be || le) {
        if (len & 1u) return false;
        for (size_t i = 0; i < len; i += 2) {
            uint32_t cp = be ? ((uint32_t)data[i] << 8) | data[i + 1] : ((uint32_t)data[i + 1] << 8) | data[i];
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 3 >= len) return false;
                uint32_t lo = le ? ((uint32_t)data[i + 3] << 8) | data[i + 2] : ((uint32_t)data[i + 2] << 8) | data[i + 3];
                if (lo < 0xDC00 || lo > 0xDFFF) return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + lo - 0xDC00;
                i += 2;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
            if (!feishu_media_append_utf8(cp, out, cap - 1, &used)) return false;
        }
    } else if (gbk) {
        for (size_t i = 0; i < len; ++i) {
            uint8_t lead = data[i]; uint32_t cp;
            if (lead < 0x80) cp = lead;
            else {
                if (lead < 0x81 || lead > 0xFE || i + 1 >= len) return false;
                uint8_t trail = data[++i]; size_t col;
                if (trail >= 0x40 && trail <= 0x7E) col = trail - 0x40;
                else if (trail >= 0x80 && trail <= 0xFE) col = 63u + trail - 0x80;
                else return false;
                if (gb2312 && (lead < 0xA1 || lead > 0xF7 || trail < 0xA1 || trail > 0xFE)) return false;
                cp = s_gbk_to_unicode[(size_t)(lead - 0x81) * 190u + col];
                if (!cp) return false;
            }
            if (!feishu_media_append_utf8(cp, out, cap - 1, &used)) return false;
        }
    } else return false;
    out[used] = '\0';
    if (out_len) *out_len = used;
    return true;
}
