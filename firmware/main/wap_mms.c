#include "wap_mms.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct { const uint8_t *p; size_t n, i; } cur_t;

static bool take(cur_t *c, size_t n, const uint8_t **p)
{
    if (n > c->n - c->i) return false;
    if (p) *p = c->p + c->i;
    c->i += n;
    return true;
}

static bool byte(cur_t *c, uint8_t *v)
{
    const uint8_t *p;
    if (!take(c, 1, &p)) return false;
    *v = *p;
    return true;
}

static bool uintvar(cur_t *c, size_t *v)
{
    size_t x = 0;
    for (unsigned i = 0; i < 5; ++i) {
        uint8_t b;
        if (!byte(c, &b) || x > (SIZE_MAX >> 7)) return false;
        x = (x << 7) | (b & 0x7f);
        if (!(b & 0x80)) { *v = x; return true; }
    }
    return false;
}

static bool text_string(cur_t *c, char *out, size_t cap)
{
    uint8_t b;
    if (!byte(c, &b)) return false;
    if (b == 0x7f && !byte(c, &b)) return false; /* WSP text-string quote */
    size_t used = 0;
    while (b) {
        if (b < 0x20 || b == 0x7f) return false;
        if (out) {
            if (used + 1 >= cap) return false;
            out[used++] = (char)b;
        }
        if (!byte(c, &b)) return false;
    }
    if (out) out[used] = 0;
    return true;
}

static bool value_length(cur_t *c, cur_t *value)
{
    uint8_t first;
    if (!byte(c, &first) || first > 31) return false;
    size_t n = first;
    const uint8_t *p;
    if ((first == 31 && !uintvar(c, &n)) || !take(c, n, &p)) return false;
    *value = (cur_t){ p, n, 0 };
    return true;
}

static bool skip_wsp_value(cur_t *c)
{
    if (c->i == c->n) return false;
    uint8_t first = c->p[c->i];
    if (first >= 0x80) return take(c, 1, NULL);
    if (first >= 32) return text_string(c, NULL, 0);
    cur_t value;
    return value_length(c, &value);
}

static bool ascii_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false;
    }
    return *a == 0 && *b == 0;
}

static bool integer_value(cur_t *c, uint64_t *value);

static const char *media_token(uint64_t t)
{
    switch (t) {
    case 0x03: return "text/plain";
    case 0x1c: return "image/*";
    case 0x1d: return "image/gif";
    case 0x1e: return "image/jpeg";
    case 0x1f: return "image/tiff";
    case 0x20: return "image/png";
    case 0x21: return "image/vnd.wap.wbmp";
    case 0x22: return "application/vnd.wap.multipart.*";
    case 0x23: return "application/vnd.wap.multipart.mixed";
    case 0x24: return "application/vnd.wap.multipart.form-data";
    case 0x25: return "application/vnd.wap.multipart.byteranges";
    case 0x26: return "application/vnd.wap.multipart.alternative";
    case 0x33: return "application/vnd.wap.multipart.related";
    case 0x3e: return "application/vnd.wap.mms-message";
    case 0x5a: return "application/octet-stream";
    default: return NULL;
    }
}

static bool media_type_direct(cur_t *c, char *out, size_t cap)
{
    if (c->i == c->n) return false;
    uint8_t first = c->p[c->i];
    if (first >= 0x80 || first <= 30) {
        uint64_t token;
        if (!integer_value(c, &token)) return false;
        const char *known = media_token(token);
        if (!known) known = "application/octet-stream";
        if (strlen(known) >= cap) return false;
        strcpy(out, known);
        return true;
    }
    if (!text_string(c, out, cap) || !*out) return false;
    for (char *p = out; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return true;
}

static bool read_charset(cur_t *c, char *out, size_t cap)
{
    if (c->i == c->n) return false;
    uint8_t first = c->p[c->i];
    if (first >= 32 && first < 0x80) return text_string(c, out, cap);
    uint64_t mib;
    if (!integer_value(c, &mib)) return false;
    const char *name;
    switch (mib) {
    case 0: name = ""; break;
    case 3: name = "US-ASCII"; break;
    case 4: name = "ISO-8859-1"; break;
    case 106: name = "UTF-8"; break;
    case 113: name = "GBK"; break;
    case 1013: name = "UTF-16BE"; break;
    case 1014: name = "UTF-16LE"; break;
    case 2025: name = "GB2312"; break;
    default: name = "unknown"; break;
    }
    if (strlen(name) >= cap) return false;
    strcpy(out, name);
    return true;
}

static bool media_type(cur_t *c, char *out, size_t cap,
                       char *charset, size_t charset_cap)
{
    if (c->i == c->n) return false;
    if (c->p[c->i] > 31) return media_type_direct(c, out, cap);
    cur_t value;
    if (!value_length(c, &value) || !media_type_direct(&value, out, cap)) return false;
    if (!charset || strcmp(out, "text/plain")) return true;
    while (value.i < value.n) {
        uint8_t first = value.p[value.i];
        bool is_charset;
        if (first >= 32 && first < 0x80) {
            char name[64];
            if (!text_string(&value, name, sizeof(name))) return false;
            is_charset = ascii_equal_ci(name, "charset");
        } else {
            uint64_t token;
            if (!integer_value(&value, &token)) return false;
            is_charset = token == 1;
        }
        if (is_charset) return read_charset(&value, charset, charset_cap);
        if (!skip_wsp_value(&value)) return false;
    }
    return true;
}

static bool copy_text_value(cur_t *c, char *out, size_t cap)
{
    /* Encoded-string-value is either Text-string or Length + Charset + Text-string. */
    if (c->i >= c->n) return false;
    uint8_t first = c->p[c->i];
    if (first >= 32) return text_string(c, out, cap);
    if (!first) return text_string(c, out, cap);
    cur_t encoded;
    if (!value_length(c, &encoded)) return false;
    uint64_t mib;
    if (!integer_value(&encoded, &mib)) return false;
    return text_string(&encoded, out, cap) && encoded.i == encoded.n;
}

static bool skip_integer(cur_t *c)
{
    uint8_t n;
    return byte(c, &n) && n <= 8 && take(c, n, NULL);
}

static bool integer_value(cur_t *c, uint64_t *value)
{
    uint8_t n;
    const uint8_t *p;
    if (!byte(c, &n)) return false;
    if (n & 0x80) { *value = n & 0x7f; return true; }
    if (n > 8 || !take(c, n, &p)) return false;
    uint64_t v = 0;
    for (uint8_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    *value = v;
    return true;
}

static bool parse_expiry(cur_t *c, wap_mms_info_t *info)
{
    uint8_t mode;
    cur_t v;
    if (!value_length(c, &v)) return false;
    if (!byte(&v, &mode)) return false;
    if (mode == 0x81) {
        info->expiry_relative = 1;
        if (!integer_value(&v, &info->expiry_seconds)) return false;
    } else if (mode == 0x80) {
        info->expiry_relative = 0;
        if (!integer_value(&v, &info->expiry_seconds)) return false;
    } else return false;
    if (v.i != v.n) return false;
    info->has_expiry = 1;
    return true;
}

static bool parse_from(cur_t *c, char *out, size_t cap)
{
    if (c->i == c->n) return false;
    cur_t value;
    bool bounded = c->p[c->i] <= 31;
    if (bounded) {
        if (!value_length(c, &value)) return false;
    } else {
        value = *c;
    }
    uint8_t token;
    if (!byte(&value, &token)) return false;
    if (token == 0x81) {
        strcpy(out, "unknown");
    } else if (token != 0x80 || !copy_text_value(&value, out, cap)) return false;
    if (bounded && value.i != value.n) return false;
    if (!bounded) c->i = value.i;
    return true;
}

static bool parse_mms_headers(cur_t *c, wap_mms_info_t *info, bool *has_body,
                              char *body_mime, char *body_charset)
{
    *has_body = false;
    while (c->i < c->n) {
        uint8_t field;
        if (!byte(c, &field)) return false;
        if (field == 0 && c->i == c->n) return true; /* Optional trailing NUL padding. */
        if (field == 0x84) { /* Content-Type starts the retrieve body. */
            if (!media_type(c, body_mime, WAP_MMS_MIME_MAX,
                            body_charset, WAP_MMS_CHARSET_MAX)) return false;
            *has_body = true;
            return true;
        }
        switch (field) {
        case 0x8c: /* X-Mms-Message-Type */
            if (info->message_type || !byte(c, &info->message_type)) return false;
            break;
        case 0x8d: /* X-Mms-MMS-Version */
            if (!byte(c, &field)) return false;
            break;
        case 0x8a: /* X-Mms-Message-Class: token or text */
            if (!skip_wsp_value(c)) return false;
            break;
        case 0x98: /* Transaction-ID */
            if (!text_string(c, info->transaction_id, sizeof(info->transaction_id))) return false;
            break;
        case 0x83: /* Content-Location */
            if (!text_string(c, info->content_location, sizeof(info->content_location))) return false;
            break;
        case 0x96: /* Subject */
            if (!copy_text_value(c, info->subject, sizeof(info->subject))) return false;
            break;
        case 0x89: /* From */
            if (!parse_from(c, info->sender, sizeof(info->sender))) return false;
            break;
        case 0x85: /* Date: long-integer */
            if (!skip_integer(c)) return false;
            break;
        case 0x8e: /* Message size */
            if (!integer_value(c, &info->message_size_bytes)) return false;
            info->has_message_size = 1;
            break;
        case 0x88: /* Expiry */
            if (!parse_expiry(c, info)) return false;
            break;
        case 0x87: /* Delivery time */
            if (!skip_wsp_value(c)) return false;
            break;
        case 0x86: /* Delivery report */
        case 0x8f: /* Priority */
        case 0x90: /* Read report */
        case 0x91: /* Report allowed */
        case 0x94: /* Sender visibility */
        case 0x95: /* Status */
        case 0x9b: /* Read status */
            if (!byte(c, &field) || field < 0x80) return false;
            break;
        case 0x92: /* Response status */
            if (info->has_response_status || !byte(c, &info->response_status) ||
                info->response_status < 0x80) return false;
            info->has_response_status = 1;
            break;
        case 0x99: /* Retrieve status */
            if (info->has_retrieve_status || !byte(c, &info->retrieve_status) ||
                info->retrieve_status < 0x80) return false;
            info->has_retrieve_status = 1;
            break;
        case 0x8b: /* Message-ID */
            if (!text_string(c, NULL, 0)) return false;
            break;
        case 0x81: /* Bcc */
        case 0x82: /* Cc */
        case 0x97: /* To */
        case 0x93: /* Response text */
        case 0x9a: /* Retrieve text */
            if (!skip_wsp_value(c)) return false;
            break;
        default:
            if (field < 0x80) {
                if (field < 0x20) return false;
                c->i--;
                if (!text_string(c, NULL, 0)) return false;
            }
            if (!skip_wsp_value(c)) return false;
            break;
        }
    }
    return true;
}

static bool read_part_headers(cur_t *h, unsigned index, wap_mms_part_t *part)
{
    char value[WAP_MMS_TEXT_MAX] = {0};
    char id[WAP_MMS_TEXT_MAX] = {0};
    while (h->i < h->n) {
        uint8_t field;
        if (!byte(h, &field)) return false;
        if (field == 0x8e) { /* Content-Location */
            if (!text_string(h, value, sizeof(value))) return false;
        } else if ((field == 0x83 || field == 0x85) && h->i < h->n &&
                   h->p[h->i] >= 32 && h->p[h->i] < 0x80) {
            /* Legacy senders use these compact fields for an attachment name. */
            if (!text_string(h, id, sizeof(id))) return false;
        } else if (field == 0xc0) { /* Content-ID */
            if (!text_string(h, id, sizeof(id))) return false;
        } else if (field >= 0x20 && field < 0x80) {
            /* Textual WSP header names are common in legacy MMS multipart. */
            h->i--;
            char name[64];
            if (!text_string(h, name, sizeof(name))) return false;
            if (ascii_equal_ci(name, "Content-Location") || ascii_equal_ci(name, "Name")) {
                if (!text_string(h, value, sizeof(value))) return false;
            } else if (ascii_equal_ci(name, "Content-ID")) {
                if (!text_string(h, id, sizeof(id))) return false;
            } else if (ascii_equal_ci(name, "Content-Transfer-Encoding")) {
                char enc[32];
                if (!text_string(h, enc, sizeof(enc))) return false;
                if (ascii_equal_ci(enc, "base64")) part->transfer_encoding = WAP_MMS_ENC_BASE64;
                else if (ascii_equal_ci(enc, "quoted-printable"))
                    part->transfer_encoding = WAP_MMS_ENC_QUOTED_PRINTABLE;
                else if (!ascii_equal_ci(enc, "binary") && !ascii_equal_ci(enc, "8bit") &&
                         !ascii_equal_ci(enc, "7bit")) return false;
                else part->transfer_encoding = WAP_MMS_ENC_NONE;
            } else if (!skip_wsp_value(h)) return false;
        } else {
            if (field < 0x80 || !skip_wsp_value(h)) return false;
        }
    }
    if (!value[0]) strcpy(value, id);
    if (!value[0]) snprintf(part->filename, sizeof(part->filename), "part%u.bin", index + 1);
    else {
        const char *base = strrchr(value, '/');
        base = base ? base + 1 : value;
        size_t n = 0;
        for (; base[n]; ++n) {
            if (n + 1 >= sizeof(part->filename)) return false;
            unsigned char ch = (unsigned char)base[n];
            part->filename[n] = (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') ? (char)ch : '_';
        }
        if (!n) return false;
        part->filename[n] = 0;
    }
    return true;
}

static int b64_value(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool decode_base64(uint8_t *p, size_t n, size_t *out_len)
{
    size_t r = 0, w = 0;
    uint8_t q[4]; unsigned qn = 0; bool ended = false;
    for (; r < n; ++r) {
        uint8_t ch = p[r];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (ended) return false;
        if (ch != '=' && b64_value(ch) < 0) return false;
        q[qn++] = ch;
        if (qn == 4) {
            int a = b64_value(q[0]), b = b64_value(q[1]);
            if (a < 0 || b < 0) return false;
            p[w++] = (uint8_t)((a << 2) | (b >> 4));
            if (q[2] == '=') {
                if (q[3] != '=') return false;
                ended = true;
            } else {
                int c = b64_value(q[2]); if (c < 0) return false;
                p[w++] = (uint8_t)((b << 4) | (c >> 2));
                if (q[3] == '=') ended = true;
                else {
                    int d = b64_value(q[3]); if (d < 0) return false;
                    p[w++] = (uint8_t)((c << 6) | d);
                }
            }
            qn = 0;
        }
    }
    if (qn) {
        int a = b64_value(q[0]);
        if (a < 0 || (qn == 1 && !w)) return false;
        if (qn >= 2) {
            int b = b64_value(q[1]);
            if (b < 0) return false;
            p[w++] = (uint8_t)((a << 2) | (b >> 4));
            if (qn == 3 && q[2] != '=') {
                int c = b64_value(q[2]);
                if (c < 0) return false;
                p[w++] = (uint8_t)((b << 4) | (c >> 2));
            }
        }
        /* Older gateways can append a single sextet. It carries no complete byte. */
    }
    *out_len = w;
    return true;
}

static int hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool decode_qp(uint8_t *p, size_t n, size_t *out_len)
{
    size_t r = 0, w = 0;
    while (r < n) {
        if (p[r] != '=') { p[w++] = p[r++]; continue; }
        if (r + 2 < n && p[r + 1] == '\r' && p[r + 2] == '\n') { r += 3; continue; }
        if (r + 1 < n && p[r + 1] == '\n') { r += 2; continue; }
        if (r + 2 >= n) return false;
        int hi = hex_digit(p[r + 1]), lo = hex_digit(p[r + 2]);
        if (hi < 0 || lo < 0) return false;
        p[w++] = (uint8_t)((hi << 4) | lo);
        r += 3;
    }
    *out_len = w;
    return true;
}

static bool forwardable_mime(const char *mime)
{
    return !strcmp(mime, "text/plain") || !strcmp(mime, "image/jpeg") ||
           !strcmp(mime, "image/png") || !strcmp(mime, "image/gif") ||
           !strcmp(mime, "image/webp") || !strcmp(mime, "image/vnd.wap.wbmp");
}

static bool parse_multipart(cur_t *c, wap_mms_retrieve_t *out, bool mutable_input)
{
    size_t count;
    if (!uintvar(c, &count) || count > 64) return false;
    for (size_t i = 0; i < count; ++i) {
        size_t headers_len, data_len;
        const uint8_t *headers, *body;
        if (!uintvar(c, &headers_len) || !uintvar(c, &data_len) ||
            headers_len > c->n - c->i || !take(c, headers_len, &headers) ||
            data_len > c->n - c->i || !take(c, data_len, &body)) return false;
        cur_t h = { headers, headers_len, 0 };
        wap_mms_part_t part = {0};
        if (!media_type(&h, part.mime, sizeof(part.mime), part.charset, sizeof(part.charset))) return false;
        if (!forwardable_mime(part.mime)) { out->ignored_parts++; continue; }
        if (out->part_count >= WAP_MMS_MAX_PARTS) { out->ignored_parts++; continue; }
        if (!part.charset[0] && !strcmp(part.mime, "text/plain") &&
            h.i + 1 < h.n && h.p[h.i] == 0x81 && h.p[h.i + 1] >= 0x80) {
            /* Keep support for senders that place charset after a bare media token. */
            h.i++;
            if (!read_charset(&h, part.charset, sizeof(part.charset))) return false;
        }
        if (!read_part_headers(&h, (unsigned)i, &part)) return false;
        part.data = body;
        part.len = data_len;
        if (part.transfer_encoding != WAP_MMS_ENC_NONE) {
            if (!mutable_input) { out->ignored_parts++; continue; }
            size_t decoded = 0;
            bool ok = part.transfer_encoding == WAP_MMS_ENC_BASE64 ?
                decode_base64((uint8_t *)body, data_len, &decoded) :
                decode_qp((uint8_t *)body, data_len, &decoded);
            if (!ok) { out->ignored_parts++; continue; }
            part.len = decoded;
            part.transfer_encoding = WAP_MMS_ENC_NONE;
        }
        out->parts[out->part_count++] = part;
    }
    return c->i == c->n;
}

wap_mms_status_t wap_mms_parse_push(const uint8_t *data, size_t len, wap_mms_info_t *out)
{
    if (!data || !out) return WAP_MMS_MALFORMED;
    memset(out, 0, sizeof(*out));
    if (!len || len > WAP_MMS_MAX_INPUT) return WAP_MMS_LIMIT;
    cur_t c = { data, len, 0 };
    uint8_t tid, pdu_type;
    size_t header_len;
    if (!byte(&c, &tid) || !byte(&c, &pdu_type) || !uintvar(&c, &header_len))
        return WAP_MMS_TRUNCATED;
    (void)tid;
    if (pdu_type != 0x06 || header_len > c.n - c.i) return WAP_MMS_NOT_MMS;
    const uint8_t *headers;
    if (!take(&c, header_len, &headers)) return WAP_MMS_TRUNCATED;
    cur_t h = { headers, header_len, 0 };
    char content_type[WAP_MMS_MIME_MAX];
    if (!media_type(&h, content_type, sizeof(content_type), NULL, 0) ||
        strcmp(content_type, "application/vnd.wap.mms-message") != 0) return WAP_MMS_NOT_MMS;
    return wap_mms_parse_notification(c.p + c.i, c.n - c.i, out);
}

wap_mms_status_t wap_mms_parse_notification(const uint8_t *data, size_t len,
                                            wap_mms_info_t *out)
{
    if (!data || !out) return WAP_MMS_MALFORMED;
    memset(out, 0, sizeof(*out));
    if (!len || len > WAP_MMS_MAX_INPUT) return WAP_MMS_LIMIT;
    cur_t c = { data, len, 0 };
    bool has_body;
    char body_mime[WAP_MMS_MIME_MAX] = {0};
    if (!parse_mms_headers(&c, out, &has_body, body_mime, NULL)) return WAP_MMS_MALFORMED;
    if (out->message_type != 0x82 || has_body) return WAP_MMS_NOT_MMS;
    return WAP_MMS_OK;
}

static wap_mms_status_t parse_retrieve_internal(const uint8_t *data, size_t len,
                                                wap_mms_retrieve_t *out, bool mutable_input)
{
    if (!data || !out) return WAP_MMS_MALFORMED;
    memset(out, 0, sizeof(*out));
    if (!len || len > WAP_MMS_MAX_INPUT) return WAP_MMS_LIMIT;
    cur_t c = { data, len, 0 };
    bool has_body;
    char body_mime[WAP_MMS_MIME_MAX] = {0};
    char body_charset[WAP_MMS_CHARSET_MAX] = {0};
    if (!parse_mms_headers(&c, &out->info, &has_body, body_mime, body_charset)) return WAP_MMS_MALFORMED;
    if (out->info.message_type != 0x84) return WAP_MMS_NOT_MMS;
    if ((out->info.has_response_status && out->info.response_status != 0x80) ||
        (out->info.has_retrieve_status && out->info.retrieve_status != 0x80))
        return WAP_MMS_RETRIEVE_FAILED;
    if (!has_body) return WAP_MMS_NOT_MMS;
    if (strncmp(body_mime, "application/vnd.wap.multipart.", 30) == 0) {
        if (!parse_multipart(&c, out, mutable_input)) return WAP_MMS_MALFORMED;
    } else {
        if (!forwardable_mime(body_mime)) { out->ignored_parts++; return WAP_MMS_OK; }
        bool text = strcmp(body_mime, "text/plain") == 0;
        wap_mms_part_t *part = &out->parts[0];
        strcpy(part->mime, body_mime);
        strcpy(part->charset, body_charset);
        strcpy(part->filename, text ? "text.txt" : "image.bin");
        part->data = c.p + c.i;
        part->len = c.n - c.i;
        out->part_count = 1;
        c.i = c.n;
    }
    return WAP_MMS_OK;
}

wap_mms_status_t wap_mms_parse_retrieve(const uint8_t *data, size_t len,
                                        wap_mms_retrieve_t *out)
{ return parse_retrieve_internal(data, len, out, false); }

wap_mms_status_t wap_mms_parse_retrieve_mutable(uint8_t *data, size_t len,
                                                wap_mms_retrieve_t *out)
{ return parse_retrieve_internal(data, len, out, true); }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

wap_mms_status_t wap_mms_hex_decode(const char *hex, size_t hex_len,
                                    uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!hex || !out || !out_len || (hex_len & 1)) return WAP_MMS_MALFORMED;
    if (hex_len / 2 > out_cap || hex_len / 2 > WAP_MMS_MAX_INPUT) return WAP_MMS_LIMIT;
    for (size_t i = 0; i < hex_len; i += 2) {
        int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return WAP_MMS_MALFORMED;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = hex_len / 2;
    return WAP_MMS_OK;
}
