#include "../feishu_media_policy.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    setbuf(stdout, NULL);
    if (feishu_media_classify("text/plain; charset=UTF-8") != FEISHU_MEDIA_TEXT ||
        feishu_media_classify("image/jpeg") != FEISHU_MEDIA_IMAGE ||
        feishu_media_classify("image/png") != FEISHU_MEDIA_IMAGE ||
        feishu_media_classify("application/pdf") != FEISHU_MEDIA_PDF ||
        feishu_media_classify("image/svg+xml") != FEISHU_MEDIA_UNSUPPORTED ||
        feishu_media_classify("text/html") != FEISHU_MEDIA_UNSUPPORTED ||
        feishu_media_classify("application/x-msdownload") != FEISHU_MEDIA_UNSUPPORTED) {
        fprintf(stderr, "media MIME policy regression\n");
        return 1;
    }
    if (strcmp(feishu_media_canonical_mime("image/jpeg; name=bad"), "image/jpeg") != 0 ||
        feishu_media_canonical_mime("image/png\r\nX-Evil: yes") != NULL) {
        fprintf(stderr, "canonical MIME header policy regression\n");
        return 10;
    }
    char name[64];
    if (!feishu_media_safe_filename("../../private photo.png", "png", name, sizeof(name)) ||
        strchr(name, '/') || strchr(name, '\\') || strcmp(name, "private_photo.png") != 0) {
        fprintf(stderr, "filename policy regression: %s\n", name);
        return 2;
    }
    if (!feishu_media_trusted_filename("../../scan.exe", "jpg", name, sizeof(name)) ||
        strcmp(name, "scan.jpg") != 0) {
        fprintf(stderr, "trusted filename extension regression: %s\n", name);
        return 11;
    }
    const uint8_t png_sig[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const uint8_t fake_png[] = { 'M', 'Z', 0, 0, 0, 0, 0, 0 };
    if (!feishu_media_payload_matches("image/png", png_sig, sizeof(png_sig)) ||
        feishu_media_payload_matches("image/png", fake_png, sizeof(fake_png)) ||
        feishu_media_payload_matches("application/pdf", fake_png, sizeof(fake_png))) {
        fprintf(stderr, "payload signature policy regression\n");
        return 12;
    }

    const uint8_t utf8[] = { 0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD };
    char decoded[64]; size_t decoded_len = 0;
    if (!feishu_media_decode_text(NULL, utf8, sizeof(utf8), decoded, sizeof(decoded), &decoded_len) ||
        decoded_len != sizeof(utf8) || memcmp(decoded, utf8, sizeof(utf8)) != 0) return 3;
    const uint8_t be[] = { 0xFE, 0xFF, 0x4F, 0x60, 0x59, 0x7D, 0xD8, 0x3D, 0xDE, 0x00 };
    const uint8_t le[] = { 0xFF, 0xFE, 0x60, 0x4F, 0x7D, 0x59, 0x3D, 0xD8, 0x00, 0xDE };
    const uint8_t utf16_utf8[] = { 0xE4,0xBD,0xA0,0xE5,0xA5,0xBD,0xF0,0x9F,0x98,0x80 };
    bool ok_be = feishu_media_decode_text("UTF-16BE", be, sizeof(be), decoded, sizeof(decoded), &decoded_len);
    if (!ok_be || decoded_len != sizeof(utf16_utf8) || memcmp(decoded, utf16_utf8, sizeof(utf16_utf8)) != 0) {
        fprintf(stderr, "UTF-16BE decode regression: ok=%d len=%zu\n", ok_be, decoded_len); return 4;
    }
    if (!feishu_media_decode_text("UTF-16LE", le, sizeof(le), decoded, sizeof(decoded), &decoded_len) ||
        decoded_len != sizeof(utf16_utf8) || memcmp(decoded, utf16_utf8, sizeof(utf16_utf8)) != 0) return 5;

    const uint8_t gbk[] = { 0xC4, 0xE3, 0xBA, 0xC3 };
    bool ok_gbk = feishu_media_decode_text("GBK", gbk, sizeof(gbk), decoded, sizeof(decoded), &decoded_len);
    if (!ok_gbk || decoded_len != sizeof(utf8) || memcmp(decoded, utf8, sizeof(utf8)) != 0) {
        fprintf(stderr, "GBK decode regression: ok=%d len=%zu\n", ok_gbk, decoded_len); return 6;
    }
    if (!feishu_media_decode_text("GB2312", gbk, sizeof(gbk), decoded, sizeof(decoded), &decoded_len) ||
        decoded_len != sizeof(utf8) || memcmp(decoded, utf8, sizeof(utf8)) != 0) return 7;
    const uint8_t latin1[] = { 'c', 'a', 'f', 0xE9 };
    const uint8_t latin1_utf8[] = { 'c', 'a', 'f', 0xC3, 0xA9 };
    if (!feishu_media_decode_text("ISO-8859-1", latin1, sizeof(latin1), decoded, sizeof(decoded), &decoded_len) ||
        decoded_len != sizeof(latin1_utf8) || memcmp(decoded, latin1_utf8, sizeof(latin1_utf8)) != 0) return 9;

    const uint8_t invalid_utf8[] = { 0xC0, 0xAF };
    const uint8_t bad_utf16[] = { 0xD8, 0x00 };
    const uint8_t invalid_gbk[] = { 0x81, 0x30 };
    if (feishu_media_decode_text("UTF-8", invalid_utf8, sizeof(invalid_utf8), decoded, sizeof(decoded), NULL) ||
        feishu_media_decode_text("UTF-16BE", bad_utf16, sizeof(bad_utf16), decoded, sizeof(decoded), NULL) ||
        feishu_media_decode_text("GBK", invalid_gbk, sizeof(invalid_gbk), decoded, sizeof(decoded), NULL) ||
        feishu_media_decode_text("unknown", utf8, sizeof(utf8), decoded, sizeof(decoded), NULL)) return 8;

    puts("PASS Feishu media MIME allowlist, safe filenames, UTF-8/UTF-16/GBK conversion");
    return 0;
}
