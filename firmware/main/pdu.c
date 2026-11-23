#include "pdu.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------- 基础工具 ---------------- */

size_t pdu_hex2bin(const char *hex, uint8_t *out, size_t max)
{
    if (!hex || !out) return 0;
    size_t chars = strlen(hex);
    if ((chars & 1) || chars / 2 > max) return 0;
    size_t n = 0;
    for (size_t i = 0; i < chars; i += 2) {
        int hi = hex[i] >= '0' && hex[i] <= '9' ? hex[i] - '0' :
                 hex[i] >= 'A' && hex[i] <= 'F' ? hex[i] - 'A' + 10 :
                 hex[i] >= 'a' && hex[i] <= 'f' ? hex[i] - 'a' + 10 : -1;
        int lo = hex[i + 1] >= '0' && hex[i + 1] <= '9' ? hex[i + 1] - '0' :
                 hex[i + 1] >= 'A' && hex[i + 1] <= 'F' ? hex[i + 1] - 'A' + 10 :
                 hex[i + 1] >= 'a' && hex[i + 1] <= 'f' ? hex[i + 1] - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) return 0;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

void pdu_bin2hex(const uint8_t *bin, size_t len, char *out)
{
    static const char *d = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = d[bin[i] >> 4];
        out[i * 2 + 1] = d[bin[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* BCD 号码解码：返回字符数，写入 out（含 '+'） */
static int bcd_number(const uint8_t *b, int octets, int toa, char *out, size_t outlen)
{
    static const char *d = "0123456789*#abc";
    int n = 0;
    if ((toa & 0x70) == 0x10 && n + 1 < (int)outlen) {
        out[n++] = '+';
    }
    for (int i = 0; i < octets; i++) {
        uint8_t lo = b[i] & 0x0F;
        uint8_t hi = (b[i] >> 4) & 0x0F;
        if (lo != 0x0F && n + 1 < (int)outlen) {
            out[n++] = d[lo];
        }
        if (hi != 0x0F && n + 1 < (int)outlen) {
            out[n++] = d[hi];
        }
    }
    out[n] = '\0';
    return n;
}

/* UTF-16BE (UCS2) -> UTF-8 */
static int ucs2_to_utf8(const uint8_t *in, int bytes, char *out, size_t outlen)
{
    int o = 0;
    for (int i = 0; i + 1 < bytes; i += 2) {
        uint32_t cp = ((uint32_t)in[i] << 8) | in[i + 1];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes) {
            uint32_t lo = ((uint32_t)in[i + 2] << 8) | in[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) {
            if (o + 1 >= (int)outlen) break;
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= (int)outlen) break;
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 >= (int)outlen) break;
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 >= (int)outlen) break;
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = '\0';
    return o;
}

/* UTF-8 -> UTF-16BE，返回字节数 */
static int utf8_to_ucs2(const char *in, uint8_t *out, size_t outlen)
{
    int o = 0;
    const uint8_t *p = (const uint8_t *)in;
    while (*p) {
        uint32_t cp;
        int extra;
        if (*p < 0x80) {
            cp = *p; extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F; extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F; extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07; extra = 3;
        } else {
            p++; continue;
        }
        p++;
        for (int i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (*p & 0x3F);
            p++;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            uint16_t hi = 0xD800 + (cp >> 10);
            uint16_t lo = 0xDC00 + (cp & 0x3FF);
            if (o + 4 > (int)outlen) break;
            out[o++] = hi >> 8; out[o++] = hi & 0xFF;
            out[o++] = lo >> 8; out[o++] = lo & 0xFF;
        } else {
            if (o + 2 > (int)outlen) break;
            out[o++] = (cp >> 8) & 0xFF;
            out[o++] = cp & 0xFF;
        }
    }
    return o;
}

/* GSM 7bit 解包：从 bit 偏移 start_bit 开始取 septets 个字符 */
static const uint16_t GSM7_BASIC[128] = {
    0x0040,0x00A3,0x0024,0x00A5,0x00E8,0x00E9,0x00F9,0x00EC,0x00F2,0x00C7,0x000A,0x00D8,0x00F8,0x000D,0x00C5,0x00E5,
    0x0394,0x005F,0x03A6,0x0393,0x039B,0x03A9,0x03A0,0x03A8,0x03A3,0x0398,0x039E,0xFFFF,0x00C6,0x00E6,0x00DF,0x00C9,
    0x0020,0x0021,0x0022,0x0023,0x00A4,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x00A1,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x00C4,0x00D6,0x00D1,0x00DC,0x00A7,
    0x00BF,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x00E4,0x00F6,0x00F1,0x00FC,0x00E0
};

static int append_utf8(uint32_t cp, char *out, int o, size_t cap)
{
    if (cp < 0x80) { if (o + 1 >= (int)cap) return -1; out[o++] = (char)cp; }
    else if (cp < 0x800) {
        if (o + 2 >= (int)cap) return -1;
        out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (o + 3 >= (int)cap) return -1;
        out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

static int gsm7_unpack(const uint8_t *data, size_t octets, int start_bit, int septets,
                       char *out, size_t outlen)
{
    int o = 0;
    for (int i = 0; i < septets; i++) {
        int bit = start_bit + i * 7;
        int byte = bit / 8;
        int shift = bit % 8;
        if (byte >= (int)octets) break;
        int v = data[byte] >> shift;
        if (shift > 1 && byte + 1 < (int)octets) {
            v |= data[byte + 1] << (8 - shift);
        }
        v &= 0x7F;
        if (v == 0x1B) {
            i++;
            bit += 7;
            byte = bit / 8;
            shift = bit % 8;
            if (byte >= (int)octets) break;
            int e = data[byte] >> shift;
            if (shift > 1 && byte + 1 < (int)octets) {
                e |= data[byte + 1] << (8 - shift);
            }
            e &= 0x7F;
            uint32_t cp = '?';
            switch (e) {
            case 0x0A: cp = 0x000C; break;
            case 0x14: cp = '^'; break;
            case 0x28: cp = '{'; break;
            case 0x29: cp = '}'; break;
            case 0x2F: cp = '\\'; break;
            case 0x3C: cp = '['; break;
            case 0x3D: cp = '~'; break;
            case 0x3E: cp = ']'; break;
            case 0x40: cp = '|'; break;
            case 0x65: cp = 0x20AC; break;
            default: break;
            }
            int next = append_utf8(cp, out, o, outlen);
            if (next < 0) break;
            o = next;
            continue;
        }
        uint32_t cp = GSM7_BASIC[v];
        if (cp == 0xFFFF) continue;
        int next = append_utf8(cp, out, o, outlen);
        if (next < 0) break;
        o = next;
    }
    if (o < (int)outlen) out[o] = '\0';
    return o;
}

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int bcd_byte(uint8_t b)
{
    int tens = b & 0x0F;
    int ones = (b >> 4) & 0x0F;
    if (tens > 9 || ones > 9) return -1;
    return tens * 10 + ones;
}

static time_t decode_scts(const uint8_t *s, int *tz_quarters)
{
    int yy = bcd_byte(s[0]);
    int mo = bcd_byte(s[1]);
    int dd = bcd_byte(s[2]);
    int hh = bcd_byte(s[3]);
    int mi = bcd_byte(s[4]);
    int ss = bcd_byte(s[5]);
    int tz_low = s[6] & 0x0F;
    int tz_high = (s[6] >> 4) & 0x0F;
    int tz_magnitude = (tz_low & 0x07) * 10 + tz_high;
    if (tz_quarters) *tz_quarters = 0;
    if (yy < 0 || mo < 1 || mo > 12 || dd < 1 || hh < 0 || hh > 23 ||
        mi < 0 || mi > 59 || ss < 0 || ss > 59 ||
        tz_high > 9 || tz_magnitude > 56) return 0;
    static const uint8_t month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int year = 2000 + yy;
    int max_day = month_days[mo - 1];
    if (mo == 2 && (year % 4 == 0)) max_day = 29;
    if (dd > max_day) return 0;
    int tz = (tz_low & 0x08) ? -tz_magnitude : tz_magnitude;
    int64_t days = days_from_civil(year, (unsigned)mo, (unsigned)dd);
    int64_t seconds = days * INT64_C(86400) + hh * INT64_C(3600) +
                      mi * INT64_C(60) + ss - (int64_t)tz * 900;
    time_t result = (time_t)seconds;
    if (seconds <= 0 || (int64_t)result != seconds) return 0;
    if (tz_quarters) *tz_quarters = tz;
    return result;
}

/* ---------------- 解码 SMS-DELIVER ---------------- */

int pdu_decode_deliver(const uint8_t *pdu, size_t len, sms_deliver_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 2) return -1;

    size_t i = 0;
    int sca_len = pdu[i++];
    if (i + (size_t)sca_len > len) return -1;
    i += sca_len;
    if (i + 2 > len) return -1;

    uint8_t first = pdu[i++];
    if ((first & 0x03) != 0x00) return -2; /* 不是 DELIVER */

    int oa_len = pdu[i++];
    if (i >= len) return -1;
    uint8_t toa = pdu[i++];
    int oa_octets = (oa_len + 1) / 2;
    if (i + oa_octets > len) return -1;
    bcd_number(pdu + i, oa_octets, toa, out->sender, sizeof(out->sender));
    i += oa_octets;

    if (i + 2 > len) return -1;
    i++; /* PID */
    out->dcs = pdu[i++];

    if (i + 7 > len) return -1;
    out->scts = decode_scts(pdu + i, &out->scts_tz_quarters);
    i += 7;

    if (i >= len) return -1;
    int udl = pdu[i++];
    size_t avail = len - i;
    bool gsm7 = ((out->dcs & 0x0C) == 0x00);
    size_t required = gsm7 ? ((size_t)udl * 7 + 7) / 8 : (size_t)udl;
    if (required > avail) return -1;
    avail = required;

    const uint8_t *ud = pdu + i;
    int udhl = 0;
    if (first & 0x40) {
        if (avail < 1) return -1;
        udhl = ud[0];
        if ((size_t)udhl + 1 > avail) return -1;
        out->has_udh = true;
        /* 解析拼接信息 */
        int p = 1;
        while (p + 1 < 1 + udhl && p + 1 < (int)avail) {
            int iei = ud[p];
            int iel = ud[p + 1];
            if (p + 2 + iel > 1 + udhl) break;
            if (iei == 0x00 && iel == 3) {
                out->concat_ref = ud[p + 2];
                out->concat_ref_width = 8;
                out->concat_total = ud[p + 3];
                out->concat_seq = ud[p + 4];
            } else if (iei == 0x08 && iel == 4) {
                out->concat_ref = (ud[p + 2] << 8) | ud[p + 3];
                out->concat_ref_width = 16;
                out->concat_total = ud[p + 4];
                out->concat_seq = ud[p + 5];
            } else if (iei == 0x05 && iel == 4) {
                int dst = (ud[p + 2] << 8) | ud[p + 3];
                if (dst == 2948) out->mms_notification = true;
            }
            p += 2 + iel;
        }
    }

    int is_ucs2 = ((out->dcs & 0x0C) == 0x08);
    if (is_ucs2) {
        int off = out->has_udh ? 1 + udhl : 0;
        if (off > (int)avail) return -1;
        ucs2_to_utf8(ud + off, (int)avail - off, out->text, sizeof(out->text));
    } else if (gsm7) {
        /* GSM7 */
        if (out->has_udh) {
            int header_septets = ((1 + udhl) * 8 + 6) / 7;
            int start_bit = header_septets * 7;
            int septets = udl - header_septets;
            if (septets < 0) septets = 0;
            gsm7_unpack(ud, avail, start_bit, septets, out->text, sizeof(out->text));
        } else {
            gsm7_unpack(ud, avail, 0, udl, out->text, sizeof(out->text));
        }
    } else {
        /* Keep WAP Push binary intact through the existing string assembly path. */
        int off = out->has_udh ? 1 + udhl : 0;
        int n = 0;
        out->binary_payload_hex = !gsm7 && out->concat_total > 1;
        if (out->mms_notification || out->binary_payload_hex) {
            static const char hex[] = "0123456789abcdef";
            for (size_t k = (size_t)off; k < avail && n + 2 < (int)sizeof(out->text); ++k) {
                out->text[n++] = hex[ud[k] >> 4];
                out->text[n++] = hex[ud[k] & 0x0f];
            }
        } else {
            /* Ordinary 8-bit SMS remains byte-preserving Latin-1-compatible. */
            for (size_t k = (size_t)off; k < avail && n + 1 < (int)sizeof(out->text); k++)
                out->text[n++] = (char)ud[k];
        }
        out->text[n] = '\0';
    }
    return 0;
}

/* ---------------- 编码 SMS-SUBMIT ---------------- */

static int encode_number(const char *num, uint8_t *out)
{
    const char *p = num;
    int toa = 0x81;
    if (*p == '+') {
        toa = 0x91;
        p++;
    }
    int digits = (int)strlen(p);
    if (digits < 1 || digits > 20) return -1;
    for (int i = 0; i < digits; ++i) if (p[i] < '0' || p[i] > '9') return -1;
    int octets = (digits + 1) / 2;
    out[0] = (uint8_t)digits;
    out[1] = (uint8_t)toa;
    for (int i = 0; i < octets; i++) {
        uint8_t lo = (uint8_t)(p[i * 2] - '0');
        uint8_t hi = (i * 2 + 1 < digits) ? (uint8_t)(p[i * 2 + 1] - '0') : 0x0F;
        out[2 + i] = (uint8_t)((hi << 4) | lo);
    }
    return 2 + octets;
}

int pdu_encode_submit(const char *sca, const char *dest, const char *text_utf8,
                      int ref, int total, int seq,
                      uint8_t *out, size_t outlen, size_t *tpdu_offset)
{
    uint8_t ud[600];
    int udl = 0;

    if (total > 1) {
        ud[udl++] = 0x05;          /* UDHL */
        ud[udl++] = 0x00;          /* IEI: concat 8bit */
        ud[udl++] = 0x03;
        ud[udl++] = (uint8_t)(ref & 0xFF);
        ud[udl++] = (uint8_t)total;
        ud[udl++] = (uint8_t)seq;
    }

    int ucs2_len = utf8_to_ucs2(text_utf8, ud + udl, sizeof(ud) - udl);
    udl += ucs2_len;

    uint8_t tmp[700];
    int o = 0;

    /* SCA：显式写入短信中心，避免部分模组把 0 长度当成"无 SMSC" */
    if (sca && *sca) {
        uint8_t num[16];
        int n = encode_number(sca, num);        /* [digits][toa][bcd...] */
        if (n < 0) return -1;
        int sca_octets = n - 1;                 /* toa + bcd */
        tmp[o++] = (uint8_t)sca_octets;
        memcpy(tmp + o, num + 1, sca_octets);
        o += sca_octets;
    } else {
        tmp[o++] = 0x00;
    }
    size_t sca_end = (size_t)o;

    if (udl > (total > 1 ? 140 : 140)) return -1;
    if ((total > 1 && ucs2_len > 134) || (total <= 1 && ucs2_len > 140)) return -1;
    tmp[o++] = (uint8_t)(0x01 | (total > 1 ? 0x40 : 0x00)); /* SUBMIT */
    tmp[o++] = 0x00;                       /* MR */
    int da_len = encode_number(dest, tmp + o);
    if (da_len < 0) return -1;
    o += da_len;
    tmp[o++] = 0x00;                       /* PID */
    tmp[o++] = 0x08;                       /* DCS: UCS2 */
    tmp[o++] = (uint8_t)udl;               /* UDL */
    memcpy(tmp + o, ud, udl);
    o += udl;

    if ((size_t)o > outlen) return -1;
    memcpy(out, tmp, o);
    if (tpdu_offset) *tpdu_offset = sca_end;
    return o - (int)sca_end;               /* TPDU 长度 */
}

/* ---------------- 自测 ---------------- */

static int check(const char *name, bool ok, char *report, size_t len, int used)
{
    int n = snprintf(report + used, len - used, "[%s] %s\n", ok ? "PASS" : "FAIL", name);
    return used + (n > 0 ? n : 0);
}

int pdu_selftest(char *report, size_t report_len)
{
    int used = 0, fails = 0;
    sms_deliver_t d;
    uint8_t buf[700];
    char hex[1400];

    /* 1. UCS2 往返 */
    size_t off = 0;
    int tpdu = pdu_encode_submit(NULL, "+1999555012345", "你好，世界", 0, 0, 0, buf, sizeof(buf), &off);
    bool ok1 = tpdu > 0;
    /* 直接构造一条 DELIVER 做解码验证 */
    uint8_t del[256];
    int n = 0;
    del[n++] = 0x00;               /* 无 SCA */
    del[n++] = 0x04;               /* DELIVER */
    n += encode_number("+1999555012345", del + n);
    del[n++] = 0x00;               /* PID */
    del[n++] = 0x08;               /* UCS2 */
    int scts_offset = n;
    uint8_t scts[7] = { 0x62, 0x11, 0x22, 0x32, 0x21, 0x03, 0x23 }; /* 2026-11-22 23:12:30 +8 */
    memcpy(del + n, scts, 7);
    n += 7;
    uint8_t body[64];
    int bl = 0;
    body[bl++] = 0x4F; body[bl++] = 0x60;  /* 你 U+4F60 */
    body[bl++] = 0x59; body[bl++] = 0x7D;  /* 好 U+597D */
    del[n++] = (uint8_t)bl;
    memcpy(del + n, body, bl);
    n += bl;

    int rc = pdu_decode_deliver(del, n, &d);
    bool ok2 = (rc == 0) && strcmp(d.sender, "+1999555012345") == 0 && strcmp(d.text, "你好") == 0;
    if (!ok2) fails++;
    used = check("UCS2 DELIVER 解码", ok2, report, report_len, used);

    /* 2. 时间戳 */
    bool ok3 = (d.scts_tz_quarters == 32) && d.scts == (time_t)1795360350;
    if (!ok3) fails++;
    used = check("SCTS 时区 +8", ok3, report, report_len, used);

    uint8_t bad_scts[256];
    memcpy(bad_scts, del, (size_t)n);
    bad_scts[scts_offset] = 0xFA; /* invalid BCD nibble */
    bool bad_scts_rejected = pdu_decode_deliver(bad_scts, (size_t)n, &d) == 0 && d.scts == 0;
    memcpy(bad_scts, del, (size_t)n);
    bad_scts[scts_offset + 6] = 0x75; /* +57 quarters exceeds 14 hours */
    bad_scts_rejected = bad_scts_rejected &&
        pdu_decode_deliver(bad_scts, (size_t)n, &d) == 0 && d.scts == 0;
    memcpy(bad_scts, del, (size_t)n);
    bad_scts[scts_offset] = 0x52; /* 2025 */
    bad_scts[scts_offset + 1] = 0x20; /* February */
    bad_scts[scts_offset + 2] = 0x92; /* February 29, non-leap year */
    bad_scts_rejected = bad_scts_rejected &&
        pdu_decode_deliver(bad_scts, (size_t)n, &d) == 0 && d.scts == 0;
    if (!bad_scts_rejected) fails++;
    used = check("SCTS rejects invalid BCD/timezone", bad_scts_rejected, report, report_len, used);

    memcpy(bad_scts, del, (size_t)n);
    const uint8_t future_scts[7] = { 0x04, 0x10, 0x20, 0x30, 0x40, 0x50, 0x00 };
    memcpy(bad_scts + scts_offset, future_scts, sizeof(future_scts)); /* 2040-01-02 03:04:05Z */
    bool future_scts_ok = pdu_decode_deliver(bad_scts, (size_t)n, &d) == 0 &&
                          d.scts == (time_t)2209086245;
    if (!future_scts_ok) fails++;
    used = check("SCTS 64-bit future epoch", future_scts_ok, report, report_len, used);

    /* 3. 长短信 UDH 编码 */
    uint8_t p1[700];
    size_t off1 = 0;
    int t1 = pdu_encode_submit(NULL, "+1999555012346", "abcdefghij", 7, 2, 1, p1, sizeof(p1), &off1);
    /* 布局: [0]=SCA长度(0) [1]=first [2]=MR [3]=DA长 [4]=TOA [5..11]=BCD [12]=PID [13]=DCS [14]=UDL [15]=UDHL */
    bool ok4 = t1 > 0 && (p1[1] & 0x40) && p1[15] == 0x05 && p1[16] == 0x00 &&
               p1[17] == 0x03 && p1[18] == 7 && p1[19] == 2 && p1[20] == 1;
    /* 用解码器验证 UDH 解析：把 SUBMIT 改成 DELIVER 不方便，直接检查字节 */
    if (!ok4) fails++;
    used = check("UDH 拼接头编码", ok4, report, report_len, used);

    /* 4. GSM7 解码 */
    /* "hello" -> 7bit 打包：E8 32 9B FD 06 */
    uint8_t g[64];
    int gn = 0;
    g[gn++] = 0x00;
    g[gn++] = 0x04;
    gn += encode_number("+1999555012345", g + gn);
    g[gn++] = 0x00;
    g[gn++] = 0x00;  /* GSM7 */
    memcpy(g + gn, scts, 7);
    gn += 7;
    const uint8_t packed[] = { 0xE8, 0x32, 0x9B, 0xFD, 0x06 };
    g[gn++] = 5;  /* septets */
    memcpy(g + gn, packed, sizeof(packed));
    gn += sizeof(packed);
    rc = pdu_decode_deliver(g, gn, &d);
    bool ok5 = (rc == 0) && strcmp(d.text, "hello") == 0;
    if (!ok5) fails++;
    used = check("GSM7 解码 hello", ok5, report, report_len, used);

    /* Greek letters use the GSM default alphabet; € uses the extension table. */
    uint8_t greek[64] = {0};
    int gr = 0;
    greek[gr++] = 0x00; greek[gr++] = 0x04;
    gr += encode_number("+1999555012345", greek + gr);
    greek[gr++] = 0x00; greek[gr++] = 0x00;
    memcpy(greek + gr, scts, 7); gr += 7;
    const uint8_t codes[] = { 0x10, 0x1B, 0x65 };
    uint8_t packed_codes[3] = {0};
    for (unsigned k = 0; k < sizeof(codes); ++k) {
        unsigned bit = k * 7;
        packed_codes[bit / 8] |= (uint8_t)(codes[k] << (bit % 8));
        if (bit % 8 > 1) packed_codes[bit / 8 + 1] |= (uint8_t)(codes[k] >> (8 - bit % 8));
    }
    greek[gr++] = sizeof(codes);
    memcpy(greek + gr, packed_codes, sizeof(packed_codes)); gr += sizeof(packed_codes);
    rc = pdu_decode_deliver(greek, gr, &d);
    bool ok_greek = rc == 0 && strcmp(d.text, "Δ€") == 0;
    if (!ok_greek) fails++;
    used = check("GSM7 Greek + extension euro", ok_greek, report, report_len, used);

    /* 5. 长短信两段合并（UDH 8bit, ref 7, total 2, seq 1/2） */
    uint8_t c[128];
    int cn = 0;
    c[cn++] = 0x00;
    c[cn++] = 0x44;
    cn += encode_number("+1999555012345", c + cn);
    c[cn++] = 0x00;
    c[cn++] = 0x08;
    memcpy(c + cn, scts, 7);
    cn += 7;
    uint8_t ud2[32];
    int ul = 0;
    ud2[ul++] = 0x0B; ud2[ul++] = 0x00; ud2[ul++] = 0x03;
    ud2[ul++] = 7; ud2[ul++] = 2; ud2[ul++] = 1;
    ud2[ul++] = 0x05; ud2[ul++] = 0x04; ud2[ul++] = 0x0B; ud2[ul++] = 0x84;
    ud2[ul++] = 0x00; ud2[ul++] = 0x00;
    ud2[ul++] = 0x4F; ud2[ul++] = 0x60; /* 你 */
    c[cn++] = (uint8_t)ul;
    memcpy(c + cn, ud2, ul);
    cn += ul;
    rc = pdu_decode_deliver(c, cn, &d);
    bool ok6 = rc == 0 && d.has_udh && d.mms_notification && d.concat_ref == 7 && d.concat_total == 2 &&
               d.concat_seq == 1 && strcmp(d.text, "你") == 0;
    if (!ok6) fails++;
    used = check("UDH 拼接解析", ok6, report, report_len, used);

    uint8_t wap[64];
    int wn = 0;
    wap[wn++] = 0x00; wap[wn++] = 0x44;
    wn += encode_number("+1999555012345", wap + wn);
    wap[wn++] = 0x00; wap[wn++] = 0x04;
    memcpy(wap + wn, scts, 7); wn += 7;
    wap[wn++] = 8;
    const uint8_t wap_ud[] = { 0x06, 0x05, 0x04, 0x0B, 0x84, 0x00, 0x00, 0x01 };
    memcpy(wap + wn, wap_ud, sizeof(wap_ud)); wn += sizeof(wap_ud);
    rc = pdu_decode_deliver(wap, wn, &d);
    bool ok_wap = rc == 0 && d.mms_notification && d.has_udh && strcmp(d.text, "01") == 0;
    if (!ok_wap) fails++;
    used = check("8-bit WAP Push port 2948", ok_wap, report, report_len, used);

    /* Reject malformed hex instead of decoding a valid-looking truncated prefix. */
    uint8_t hx[4];
    bool ok7 = pdu_hex2bin("00A1", hx, sizeof(hx)) == 2 && hx[0] == 0 && hx[1] == 0xA1 &&
               pdu_hex2bin("00A", hx, sizeof(hx)) == 0 &&
               pdu_hex2bin("00XZ", hx, sizeof(hx)) == 0 &&
               pdu_hex2bin("0011223344", hx, sizeof(hx)) == 0;
    if (!ok7) fails++;
    used = check("PDU hex 严格校验", ok7, report, report_len, used);

    bool ok8 = pdu_encode_submit(NULL, "12A45", "x", 0, 1, 1, buf, sizeof(buf), &off) < 0;
    if (!ok8) fails++;
    used = check("拒绝非法目的号码", ok8, report, report_len, used);

    (void)hex; (void)ok1;
    return fails;
}
