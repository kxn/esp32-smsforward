/* Pure logic behind peer_mark.h: URL template expansion, tolerant mark-label
 * extraction from a provider JSON body, and display-label joining.
 *
 * Deliberately free of ESP-IDF, cJSON, heap and I/O, and free of static mutable
 * state, so the exact same code runs on the device and in the host tests. */

#include "peer_mark_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A hostile body must not be able to recurse without bound. */
#define PM_MAX_DEPTH 32u

/* ------------------------------------------------------------------ */
/* generic helpers                                                     */
/* ------------------------------------------------------------------ */

static bool pm_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool pm_hex_digit(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Strict enough to reject overlong forms, surrogates and > U+10FFFF, which is
 * what "never split a UTF-8 sequence" needs: an invalid tail is dropped rather
 * than pasted into the display text. */
static bool pm_utf8_valid(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t extra;
        uint32_t cp;
        if (c < 0x80) {
            if (c == 0) return false; /* cannot live in a C string label */
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            cp = (uint32_t)(c & 0x1Fu);
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            cp = (uint32_t)(c & 0x0Fu);
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            cp = (uint32_t)(c & 0x07u);
        } else {
            return false;
        }
        if (i + extra >= len) return false;
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }
        if (extra == 1 && cp < 0x80u) return false;
        if (extra == 2 && (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))) return false;
        if (extra == 3 && (cp < 0x10000u || cp > 0x10FFFFu)) return false;
        i += extra + 1;
    }
    return true;
}

/* Values providers use to say "nothing known about this number". */
static const char *const PM_PLACEHOLDERS[] = {
    "未标记", "无标记", "无", "未知", "正常", "普通",
    "normal", "none", "null", "unknown", "no mark",
};

static bool pm_label_is_placeholder(const char *s, size_t len)
{
    const size_t count = sizeof(PM_PLACEHOLDERS) / sizeof(PM_PLACEHOLDERS[0]);
    if (len == 0) return true;
    for (size_t i = 0; i < count; ++i) {
        const char *p = PM_PLACEHOLDERS[i];
        size_t n = strlen(p);
        if (n != len) continue;
        size_t k = 0;
        while (k < n) {
            unsigned char a = (unsigned char)s[k];
            unsigned char b = (unsigned char)p[k];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
            k++;
        }
        if (k == n) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* A. template expansion                                               */
/* ------------------------------------------------------------------ */

static bool pm_url_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

static size_t pm_encoded_len(const char *value)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p != 0; ++p) {
        n += pm_url_unreserved(*p) ? 1u : 3u;
    }
    return n;
}

static void pm_encode_into(char **dst, const char *value)
{
    const char *hex = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)value; *p != 0; ++p) {
        unsigned char c = *p;
        if (pm_url_unreserved(c)) {
            *(*dst)++ = (char)c;
        } else {
            *(*dst)++ = '%';
            *(*dst)++ = hex[c >> 4];
            *(*dst)++ = hex[c & 0x0Fu];
        }
    }
}

static bool pm_has_number_placeholder(const char *tmpl)
{
    for (const char *p = tmpl; *p != '\0'; ++p) {
        if (strncmp(p, "{number}", 8) == 0) return true;
    }
    return false;
}

size_t peer_mark_build_url(char *out, size_t cap, const char *tmpl,
                           const char *key, const char *number)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (tmpl == NULL || tmpl[0] == '\0') return 0;
    if (number == NULL || number[0] == '\0') return 0;
    if (!pm_has_number_placeholder(tmpl)) return 0;

    /* Measure first so a too-small buffer never sees a partial URL. */
    size_t need = 0;
    for (const char *p = tmpl; *p != '\0';) {
        if (strncmp(p, "{number}", 8) == 0) {
            need += pm_encoded_len(number);
            p += 8;
        } else if (strncmp(p, "{key}", 5) == 0) {
            if (key != NULL && key[0] != '\0') need += pm_encoded_len(key);
            p += 5;
        } else {
            need++;
            p++;
        }
        if (need >= cap) return 0; /* also keeps 'need' from overflowing */
    }

    char *dst = out;
    for (const char *p = tmpl; *p != '\0';) {
        if (strncmp(p, "{number}", 8) == 0) {
            pm_encode_into(&dst, number);
            p += 8;
        } else if (strncmp(p, "{key}", 5) == 0) {
            if (key != NULL && key[0] != '\0') pm_encode_into(&dst, key);
            p += 5;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0'; /* the measuring pass reserved room for it */
    return (size_t)(dst - out);
}

/* ------------------------------------------------------------------ */
/* B. tolerant label extraction                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char buf[PEER_MARK_LABEL_MAX];
    size_t len;
    bool overflow;
} pm_buf;

typedef void (*pm_emit_fn)(void *ctx, unsigned char byte);

static void pm_buf_put(void *ctx, unsigned char byte)
{
    pm_buf *b = (pm_buf *)ctx;
    if (b->len >= sizeof(b->buf) - 1u) {
        b->overflow = true;
        return;
    }
    b->buf[b->len++] = (char)byte;
}

static void pm_emit_drop(void *ctx, unsigned char byte)
{
    (void)ctx;
    (void)byte;
}

typedef struct {
    const char *s;
    size_t len;
    size_t i;
    unsigned depth;
    bool tier1_only;
    bool stopped;
    size_t nlabels;
    char labels[PEER_MARK_LABELS_MAX][PEER_MARK_LABEL_MAX];
} pm_scan;

static void pm_scan_add(pm_scan *sc, const char *raw, size_t len)
{
    while (len > 0 && pm_space((unsigned char)raw[0])) {
        raw++;
        len--;
    }
    while (len > 0 && pm_space((unsigned char)raw[len - 1])) len--;
    if (len == 0) return;
    if (len > (size_t)(PEER_MARK_LABEL_MAX - 1)) return; /* skip, never truncate */
    if (!pm_utf8_valid(raw, len)) return;
    if (pm_label_is_placeholder(raw, len)) return;
    for (size_t i = 0; i < sc->nlabels; ++i) {
        if (strlen(sc->labels[i]) == len && memcmp(sc->labels[i], raw, len) == 0) {
            return; /* exact duplicate */
        }
    }
    if (sc->nlabels >= (size_t)PEER_MARK_LABELS_MAX) return;
    memcpy(sc->labels[sc->nlabels], raw, len);
    sc->labels[sc->nlabels][len] = '\0';
    sc->nlabels++;
    if (sc->nlabels >= (size_t)PEER_MARK_LABELS_MAX) sc->stopped = true;
}

/* Reads exactly four hex digits at the cursor, advancing past them. */
static bool pm_read_hex4(pm_scan *sc, unsigned *out)
{
    unsigned v = 0;
    for (unsigned k = 0; k < 4u; ++k) {
        if (sc->i >= sc->len) return false;
        unsigned char c = (unsigned char)sc->s[sc->i];
        if (!pm_hex_digit(c)) return false;
        unsigned d = (c <= '9') ? (unsigned)(c - '0') : (unsigned)((c | 0x20) - 'a' + 10);
        v = (v << 4) | d;
        sc->i++;
    }
    *out = v;
    return true;
}

/* Appends one code point as UTF-8. */
static void pm_emit_codepoint(pm_emit_fn emit, void *ctx, unsigned cp)
{
    if (cp < 0x80u) {
        emit(ctx, (unsigned char)cp);
    } else if (cp < 0x800u) {
        emit(ctx, (unsigned char)(0xC0u | (cp >> 6)));
        emit(ctx, (unsigned char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        emit(ctx, (unsigned char)(0xE0u | (cp >> 12)));
        emit(ctx, (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)));
        emit(ctx, (unsigned char)(0x80u | (cp & 0x3Fu)));
    } else {
        emit(ctx, (unsigned char)(0xF0u | (cp >> 18)));
        emit(ctx, (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu)));
        emit(ctx, (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)));
        emit(ctx, (unsigned char)(0x80u | (cp & 0x3Fu)));
    }
}

/* Decode a JSON string, emitting decoded bytes.  Returns false on truncated or
 * otherwise unterminated input.  Escapes are handled here so the scanners never
 * see `\"` as a closing quote. */
static bool pm_decode_string(pm_scan *sc, pm_emit_fn emit, void *ctx, bool *bad)
{
    if (sc->i >= sc->len || sc->s[sc->i] != '"') return false;
    sc->i++;
    while (sc->i < sc->len) {
        unsigned char c = (unsigned char)sc->s[sc->i++];
        if (c == '"') return true;
        if (c == '\\') {
            if (sc->i >= sc->len) return false;
            unsigned char e = (unsigned char)sc->s[sc->i++];
            switch (e) {
            case '"': emit(ctx, (unsigned char)'"'); break;
            case '\\': emit(ctx, (unsigned char)'\\'); break;
            case '/': emit(ctx, (unsigned char)'/'); break;
            case 'n': emit(ctx, (unsigned char)'\n'); break;
            case 't': emit(ctx, (unsigned char)'\t'); break;
            case 'r': emit(ctx, (unsigned char)'\r'); break;
            case 'b': emit(ctx, (unsigned char)'\b'); break;
            case 'f': emit(ctx, (unsigned char)'\f'); break;
            case 'u': {
                /* \uXXXX is how several Chinese providers escape labels, so decode
                 * it; a lone surrogate or NUL means the value is not text. */
                unsigned cp = 0;
                if (!pm_read_hex4(sc, &cp)) return false;
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    unsigned lo = 0;
                    if (sc->i + 1 < sc->len && sc->s[sc->i] == '\\' && sc->s[sc->i + 1] == 'u') {
                        sc->i += 2;
                        if (!pm_read_hex4(sc, &lo)) return false;
                        if (lo < 0xDC00u || lo > 0xDFFFu) {
                            *bad = true;
                            break;
                        }
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    } else {
                        *bad = true;
                        break;
                    }
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    *bad = true;
                    break;
                } else if (cp == 0) {
                    *bad = true;
                    break;
                }
                pm_emit_codepoint(emit, ctx, cp);
                break;
            }
            default:
                *bad = true;
                break;
            }
        } else if (c == 0) {
            *bad = true; /* cannot be represented in the output */
        } else {
            emit(ctx, c);
        }
    }
    return false; /* ran out of input inside the string */
}

static bool pm_skip_string(pm_scan *sc)
{
    bool bad = false;
    return pm_decode_string(sc, pm_emit_drop, NULL, &bad);
}

/* Consume a JSON number / true / false / null token without interpreting it. */
static bool pm_skip_scalar(pm_scan *sc)
{
    size_t start = sc->i;
    while (sc->i < sc->len) {
        char c = sc->s[sc->i];
        if (pm_space((unsigned char)c) || c == ',' || c == '}' || c == ']') break;
        sc->i++;
    }
    return sc->i > start;
}

static bool pm_parse_value(pm_scan *sc, bool in_tags_elem);
static bool pm_parse_object(pm_scan *sc, bool in_tags_elem);
static bool pm_parse_array(pm_scan *sc, bool tags_array);

static bool pm_key_is(const pm_buf *key, bool bad_key, const char *lit)
{
    size_t n = strlen(lit);
    return !bad_key && !key->overflow && key->len == n && memcmp(key->buf, lit, n) == 0;
}

/* "tags": [ {"name": ...} ] -- the highest-priority shape. */
static bool pm_member_name(pm_scan *sc)
{
    if (sc->i < sc->len && sc->s[sc->i] == '"') {
        pm_buf v;
        bool bad = false;
        memset(&v, 0, sizeof(v));
        if (!pm_decode_string(sc, pm_buf_put, &v, &bad)) return false;
        if (!bad && !v.overflow) pm_scan_add(sc, v.buf, v.len);
        return true;
    }
    return pm_parse_value(sc, false);
}

static bool pm_member_string(pm_scan *sc)
{
    return pm_member_name(sc);
}

/* "catnames": "快递送餐,外卖/骚扰" -- every non-empty piece is a label. */
typedef struct {
    pm_scan *sc;
    char buf[PEER_MARK_LABEL_MAX];
    size_t len;
    bool too_long;
    unsigned char t0;
    unsigned char t1;
    unsigned char t2;
} pm_split;

static void pm_split_flush(pm_split *sp)
{
    if (!sp->too_long && sp->len > 0) pm_scan_add(sp->sc, sp->buf, sp->len);
    sp->len = 0;
    sp->too_long = false;
    sp->t0 = 0;
    sp->t1 = 0;
    sp->t2 = 0;
}

static void pm_split_put(void *ctx, unsigned char byte)
{
    pm_split *sp = (pm_split *)ctx;
    sp->t0 = sp->t1;
    sp->t1 = sp->t2;
    sp->t2 = byte;
    /* U+FF0C FULLWIDTH COMMA is EF BC 8C; its bytes were emitted one by one. */
    if (sp->t0 == (unsigned char)0xEF && sp->t1 == (unsigned char)0xBC &&
        sp->t2 == (unsigned char)0x8C) {
        /* The two lead bytes were already buffered; the third is not emitted. */
        if (sp->len >= 2u) sp->len -= 2u;
        else sp->len = 0;
        pm_split_flush(sp);
        return;
    }
    if (byte == (unsigned char)',' || byte == (unsigned char)'/') {
        pm_split_flush(sp);
        return;
    }
    if (sp->len >= sizeof(sp->buf) - 1u) {
        sp->too_long = true;
        return;
    }
    sp->buf[sp->len++] = (char)byte;
}

static bool pm_member_catnames(pm_scan *sc)
{
    if (sc->i >= sc->len || sc->s[sc->i] != '"') return pm_parse_value(sc, false);
    pm_split sp;
    memset(&sp, 0, sizeof(sp));
    sp.sc = sc;
    bool bad = false;
    if (!pm_decode_string(sc, pm_split_put, &sp, &bad)) return false;
    if (bad) return true; /* unsupported escape: drop the whole value */
    pm_split_flush(&sp);
    return true;
}

/* JUHE-style numeric mark ids (来电号码显示 / 号码标记). */
static const char *const PM_FID_LABELS[7] = {
    "骚扰电话", "推销", "中介", "快递送餐", "疑似诈骗", "招聘猎头", "出租车司机",
};

static bool pm_member_fid(pm_scan *sc)
{
    size_t start = sc->i;
    bool negative = false;
    unsigned long value = 0;
    size_t digits = 0;

    if (sc->i < sc->len && sc->s[sc->i] == '-') {
        negative = true;
        sc->i++;
    }
    while (sc->i < sc->len && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9') {
        if (value < 1000UL) value = value * 10UL + (unsigned long)(sc->s[sc->i] - '0');
        sc->i++;
        digits++;
    }
    if (digits == 0) {
        sc->i = start;
        return pm_parse_value(sc, false);
    }
    bool plain = true;
    if (sc->i < sc->len) {
        char c = sc->s[sc->i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (c == '.' || c == 'e' || c == 'E' || alpha) plain = false;
    }
    if (!plain) {
        /* A fractional/exponent/odd token is not an integer mark id. */
        while (sc->i < sc->len) {
            char c = sc->s[sc->i];
            if (pm_space((unsigned char)c) || c == ',' || c == '}' || c == ']') break;
            sc->i++;
        }
        return true;
    }
    if (!negative && digits <= 3 && value >= 1 && value <= 7) {
        pm_scan_add(sc, PM_FID_LABELS[value - 1], strlen(PM_FID_LABELS[value - 1]));
    }
    return true;
}

static bool pm_parse_member(pm_scan *sc, const pm_buf *key, bool bad_key, bool in_tags_elem)
{
    if (pm_key_is(key, bad_key, "tags")) {
        if (sc->i < sc->len && sc->s[sc->i] == '[') return pm_parse_array(sc, true);
        return pm_parse_value(sc, false);
    }
    if (in_tags_elem && pm_key_is(key, bad_key, "name")) {
        if (!sc->tier1_only) return pm_parse_value(sc, false);
        return pm_member_name(sc);
    }
    if (!sc->tier1_only) {
        if (pm_key_is(key, bad_key, "tag") || pm_key_is(key, bad_key, "mark") ||
            pm_key_is(key, bad_key, "label") || pm_key_is(key, bad_key, "category")) {
            return pm_member_string(sc);
        }
        if (pm_key_is(key, bad_key, "catnames") || pm_key_is(key, bad_key, "catname")) {
            return pm_member_catnames(sc);
        }
        if (pm_key_is(key, bad_key, "fid")) return pm_member_fid(sc);
    }
    return pm_parse_value(sc, false);
}

static bool pm_parse_object(pm_scan *sc, bool in_tags_elem)
{
    if (++sc->depth > PM_MAX_DEPTH) return false;
    if (sc->i >= sc->len || sc->s[sc->i] != '{') return false;
    sc->i++;
    while (sc->i < sc->len && pm_space((unsigned char)sc->s[sc->i])) sc->i++;
    if (sc->i < sc->len && sc->s[sc->i] == '}') {
        sc->i++;
        sc->depth--;
        return true;
    }
    for (;;) {
        while (sc->i < sc->len && pm_space((unsigned char)sc->s[sc->i])) sc->i++;
        pm_buf key;
        bool bad_key = false;
        memset(&key, 0, sizeof(key));
        if (!pm_decode_string(sc, pm_buf_put, &key, &bad_key)) return false;
        while (sc->i < sc->len && pm_space((unsigned char)sc->s[sc->i])) sc->i++;
        if (sc->i >= sc->len || sc->s[sc->i] != ':') return false;
        sc->i++;
        while (sc->i < sc->len && pm_space((unsigned char)sc->s[sc->i])) sc->i++;
        if (!pm_parse_member(sc, &key, bad_key, in_tags_elem)) return false;
        if (sc->stopped) return false;
        while (sc->i < sc->len && pm_space((unsigned char)sc->s[sc->i])) sc->i++;
        if (sc->i >= sc->len) return false;
        char delim = sc->s[sc->i];
        if (delim == ',') {
            sc->i++;
            continue;
        }
        if (delim == '}') {
            sc->i++;
            sc->depth--;
            return true;
        }
        return false;
    }
}

static bool pm_parse_array(pm_scan *sc, bool tags_array)
{
    if (++sc->depth > PM_MAX_DEPTH) return false;
    if (sc->i >= sc->len || sc->s[sc->i] != '[') return false;
    sc->i++;
    while (sc->i < sc->len && pm_space((unsigned char)sc->s[sc->i])) sc->i++;
    if (sc->i < sc->len && sc->s[sc->i] == ']') {
        sc->i++;
        sc->depth--;
        return true;
    }
    for (;;) {
        while (sc->i < sc->len && pm_space((unsigned char)sc->s[sc->i])) sc->i++;
        if (sc->i >= sc->len) return false;
        char c = sc->s[sc->i];
        bool ok;
        if (tags_array && c == '"') {
            ok = sc->tier1_only ? pm_skip_string(sc) : pm_member_string(sc);
        } else if (c == '{') {
            ok = pm_parse_object(sc, tags_array);
        } else if (c == '[') {
            ok = pm_parse_array(sc, false);
        } else if (c == '"') {
            ok = pm_skip_string(sc);
        } else {
            ok = pm_skip_scalar(sc);
        }
        if (!ok) return false;
        if (sc->stopped) return false;
        while (sc->i < sc->len && pm_space((unsigned char)sc->s[sc->i])) sc->i++;
        if (sc->i >= sc->len) return false;
        char delim = sc->s[sc->i];
        if (delim == ',') {
            sc->i++;
            continue;
        }
        if (delim == ']') {
            sc->i++;
            sc->depth--;
            return true;
        }
        return false;
    }
}

static bool pm_parse_value(pm_scan *sc, bool in_tags_elem)
{
    if (sc->i >= sc->len) return false;
    char c = sc->s[sc->i];
    if (c == '{') return pm_parse_object(sc, in_tags_elem);
    if (c == '[') return pm_parse_array(sc, false);
    if (c == '"') return pm_skip_string(sc);
    return pm_skip_scalar(sc);
}

/* One full pass.  Returns the syntax verdict; labels are reported separately so
 * a truncated body can still contribute what it already yielded. */
static bool pm_scan_run(const char *json, size_t len, bool tier1_only, pm_scan *out)
{
    memset(out, 0, sizeof(*out));
    out->s = json;
    out->len = len;
    out->tier1_only = tier1_only;
    while (out->i < out->len && pm_space((unsigned char)out->s[out->i])) out->i++;
    bool ok = pm_parse_value(out, false);
    if (ok) {
        while (out->i < out->len && pm_space((unsigned char)out->s[out->i])) out->i++;
        if (out->i != out->len) ok = false; /* trailing garbage */
    }
    return ok;
}

bool peer_mark_parse_labels(const char *json, size_t len, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    if (json == NULL || len == 0) return false;

    pm_scan first;
    pm_scan second;
    const pm_scan *found = &first;
    (void)pm_scan_run(json, len, true, &first);
    if (first.nlabels == 0) {
        (void)pm_scan_run(json, len, false, &second);
        found = &second;
    }
    if (found->nlabels == 0) return false;

    size_t pos = 0;
    for (size_t i = 0; i < found->nlabels; ++i) {
        size_t l = strlen(found->labels[i]);
        size_t sep = (pos > 0) ? 1u : 0u;
        if (pos + sep + l + 1u > cap) break; /* drop this and trailing labels */
        if (sep != 0) out[pos++] = '/';
        memcpy(out + pos, found->labels[i], l);
        pos += l;
    }
    if (pos == 0) {
        out[0] = '\0';
        return false;
    }
    out[pos] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* C. display label joining                                            */
/* ------------------------------------------------------------------ */

size_t peer_mark_join_label(char *out, size_t cap, const char *region, const char *mark)
{
    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';

    const bool has_region = region != NULL && region[0] != '\0';
    const bool has_mark = mark != NULL && mark[0] != '\0';
    if (!has_region && !has_mark) return 0;

    struct pm_seg {
        const char *p;
        size_t n;
    };
    struct pm_seg segs[3];
    size_t nseg = 0;
    if (has_region) {
        segs[nseg].p = region;
        segs[nseg].n = strlen(region);
        nseg++;
    }
    if (has_region && has_mark) {
        segs[nseg].p = " \xC2\xB7 "; /* space, U+00B7 MIDDLE DOT, space */
        segs[nseg].n = 4;
        nseg++;
    }
    if (has_mark) {
        segs[nseg].p = mark;
        segs[nseg].n = strlen(mark);
        nseg++;
    }

    size_t total = 0;
    for (size_t i = 0; i < nseg; ++i) total += segs[i].n;

    const size_t limit = cap - 1;
    size_t n = total < limit ? total : limit;
    if (n < total) {
        /* Back off to the last complete UTF-8 sequence: the first excluded byte
         * must not be a continuation byte. */
        while (n > 0) {
            size_t off = n;
            size_t k = 0;
            while (k < nseg && off >= segs[k].n) {
                off -= segs[k].n;
                k++;
            }
            if (k >= nseg) break;
            unsigned char b = (unsigned char)segs[k].p[off];
            if ((b & 0xC0u) != 0x80u) break;
            n--;
        }
    }

    size_t pos = 0;
    for (size_t i = 0; i < nseg && pos < n; ++i) {
        size_t take = segs[i].n;
        if (take > n - pos) take = n - pos;
        memcpy(out + pos, segs[i].p, take);
        pos += take;
    }
    out[pos] = '\0';
    return pos;
}

size_t peer_mark_normalise_number(const char *in, char *out, size_t cap)
{
    if (!in || !out || cap < 2) return 0;
    size_t n = 0;
    for (const char *p = in; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        if (n + 2 > cap) return 0;
        out[n++] = *p;
    }
    if (n == 0) return 0;
    size_t skip = 0;
    if (n >= 4 && strncmp(out, "0086", 4) == 0 && n - 4 >= 7) skip = 4;
    else if (n == 13 && strncmp(out, "86", 2) == 0) skip = 2;
    if (skip) {
        n -= skip;
        memmove(out, out + skip, n);
    }
    out[n] = '\0';
    return n;
}
