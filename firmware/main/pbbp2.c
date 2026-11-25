#include "pbbp2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int put_varint(uint8_t *out, uint64_t v)
{
    int n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v);
    return n;
}

static int get_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *v)
{
    uint64_t r = 0;
    int shift = 0;
    while (*pos < len && shift < 64) {
        uint8_t b = buf[(*pos)++];
        if (shift == 63 && (b & 0x7E)) return -1;
        r |= ((uint64_t)(b & 0x7F)) << shift;
        if (!(b & 0x80)) {
            *v = r;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

static int put_tag(uint8_t *out, int field, int wire)
{
    return put_varint(out, ((uint64_t)field << 3) | (uint64_t)wire);
}

static int put_bytes(uint8_t *out, size_t outlen, int field, const void *data, size_t n)
{
    int o = 0;
    uint8_t tmp[8];
    int tl = put_tag(tmp, field, 2);
    if (!out || (!data && n) || n > outlen || n > INT_MAX - 10 ||
        (size_t)tl + 10 + n > outlen) return -1;
    memcpy(out + o, tmp, tl);
    o += tl;
    o += put_varint(out + o, n);
    memcpy(out + o, data, n);
    return o + (int)n;
}

void pbbp2_frame_init(pbbp2_frame_t *f)
{
    memset(f, 0, sizeof(*f));
    f->method = 1;
}

void pbbp2_frame_free(pbbp2_frame_t *f)
{
    for (int i = 0; i < f->header_count; i++) {
        free(f->headers[i].key);
        free(f->headers[i].value);
    }
    free(f->payload);
    f->header_count = 0;
    f->payload = NULL;
    f->payload_len = 0;
}

const char *pbbp2_header_get(const pbbp2_frame_t *f, const char *key)
{
    for (int i = 0; i < f->header_count; i++) {
        if (strcmp(f->headers[i].key, key) == 0) return f->headers[i].value;
    }
    return NULL;
}

int pbbp2_header_set(pbbp2_frame_t *f, const char *key, const char *value)
{
    /* A header without both halves is not encodable: pbbp2_encode() would call
     * strlen(NULL) and the frame would be built from whatever followed it. */
    if (!f || !key || !value) return -1;
    for (int i = 0; i < f->header_count; i++) {
        if (strcmp(f->headers[i].key, key) == 0) {
            char *copy = strdup(value);
            if (!copy) return -1;
            free(f->headers[i].value);
            f->headers[i].value = copy;
            return 0;
        }
    }
    if (f->header_count >= PBBP2_MAX_HEADERS) return -1;
    char *k = strdup(key);
    if (!k) return -1;
    char *v = strdup(value);
    if (!v) {
        free(k);
        return -1;
    }
    f->headers[f->header_count].key = k;
    f->headers[f->header_count].value = v;
    f->header_count++;
    return 0;
}

int pbbp2_encode(const pbbp2_frame_t *f, uint8_t *out, size_t outlen)
{
    if (!f || !out || outlen < 32 || f->header_count < 0 || f->header_count > PBBP2_MAX_HEADERS) return -1;
    int o = 0;
    uint8_t tmp[16];
    int n;

    n = put_tag(tmp, 1, 0); if ((size_t)(n+10)>outlen-(size_t)o) return -1; memcpy(out + o, tmp, n); o += n;
    n = put_varint(out + o, f->seq_id); o += n;
    n = put_tag(tmp, 2, 0); if ((size_t)(n+10)>outlen-(size_t)o) return -1; memcpy(out + o, tmp, n); o += n;
    n = put_varint(out + o, f->log_id); o += n;
    n = put_tag(tmp, 3, 0); if ((size_t)(n+10)>outlen-(size_t)o) return -1; memcpy(out + o, tmp, n); o += n;
    n = put_varint(out + o, (uint64_t)(int64_t)f->service); o += n;
    n = put_tag(tmp, 4, 0); if ((size_t)(n+10)>outlen-(size_t)o) return -1; memcpy(out + o, tmp, n); o += n;
    n = put_varint(out + o, (uint64_t)(int64_t)f->method); o += n;

    for (int i = 0; i < f->header_count; i++) {
        uint8_t h[512];
        int ho = 0;
        int a = put_bytes(h + ho, sizeof(h) - ho, 1, f->headers[i].key, strlen(f->headers[i].key));
        if (a < 0) return -1;
        ho += a;
        a = put_bytes(h + ho, sizeof(h) - ho, 2, f->headers[i].value, strlen(f->headers[i].value));
        if (a < 0) return -1;
        ho += a;
        int w = put_bytes(out + o, outlen - o, 5, h, (size_t)ho);
        if (w < 0) return -1;
        o += w;
    }

    if (f->payload_encoding[0]) {
        int w = put_bytes(out + o, outlen - o, 6, f->payload_encoding, strlen(f->payload_encoding));
        if (w < 0) return -1;
        o += w;
    }
    if (f->payload_type[0]) {
        int w = put_bytes(out + o, outlen - o, 7, f->payload_type, strlen(f->payload_type));
        if (w < 0) return -1;
        o += w;
    }
    if (f->payload && f->payload_len) {
        int w = put_bytes(out + o, outlen - o, 8, f->payload, f->payload_len);
        if (w < 0) return -1;
        o += w;
    }
    if (f->log_id_new[0]) {
        int w = put_bytes(out + o, outlen - o, 9, f->log_id_new, strlen(f->log_id_new));
        if (w < 0) return -1;
        o += w;
    }
    return o;
}

static int parse_header(const uint8_t *buf, size_t len, pbbp2_header_t *h)
{
    size_t p = 0;
    h->key = NULL;
    h->value = NULL;
    while (p < len) {
        uint64_t tag;
        if (get_varint(buf, len, &p, &tag) < 0) return -1;
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 7);
        if (wire != 2) return -1;
        uint64_t n;
        if (get_varint(buf, len, &p, &n) < 0 || n > len - p) goto fail;
        char *s = malloc(n + 1);
        if (!s) goto fail;
        memcpy(s, buf + p, n);
        s[n] = '\0';
        p += n;
        if (field == 1) { free(h->key); h->key = s; }
        else if (field == 2) { free(h->value); h->value = s; }
        else free(s);
    }
    if (h->key && h->value) return 0;
fail:
    free(h->key); free(h->value); h->key = NULL; h->value = NULL;
    return -1;
}

int pbbp2_decode(const uint8_t *buf, size_t len, pbbp2_frame_t *f)
{
    if (!buf || !f) return -1;
    size_t p = 0;
    pbbp2_frame_init(f);
    while (p < len) {
        uint64_t tag;
        if (get_varint(buf, len, &p, &tag) < 0) goto fail;
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 7);

        if (wire == 0) {
            uint64_t v;
            if (get_varint(buf, len, &p, &v) < 0) goto fail;
            switch (field) {
            case 1: f->seq_id = v; break;
            case 2: f->log_id = v; break;
            case 3: f->service = (int32_t)v; break;
            case 4: f->method = (int32_t)v; break;
            default: break;
            }
        } else if (wire == 2) {
            uint64_t n;
            if (get_varint(buf, len, &p, &n) < 0 || n > len - p) goto fail;
            const uint8_t *d = buf + p;
            p += n;
            switch (field) {
            case 5: {
                if (f->header_count >= PBBP2_MAX_HEADERS) break;
                pbbp2_header_t h;
                if (parse_header(d, (size_t)n, &h) == 0) {
                    f->headers[f->header_count++] = h;
                }
                break;
            }
            case 6:
                snprintf(f->payload_encoding, sizeof(f->payload_encoding), "%.*s", (int)n, (const char *)d);
                break;
            case 7:
                snprintf(f->payload_type, sizeof(f->payload_type), "%.*s", (int)n, (const char *)d);
                break;
            case 8:
                /* A repeated payload field replaces the previous one; freeing it
                 * first keeps a hostile or unusual frame from leaking memory. */
                free(f->payload);
                f->payload = NULL;
                f->payload_len = 0;
                f->payload = malloc((size_t)n + 1);
                if (!f->payload) goto fail;
                memcpy(f->payload, d, (size_t)n);
                f->payload[n] = 0;
                f->payload_len = (size_t)n;
                break;
            case 9:
                snprintf(f->log_id_new, sizeof(f->log_id_new), "%.*s", (int)n, (const char *)d);
                break;
            default:
                break;
            }
        } else if (wire == 5) {
            if (len - p < 4) goto fail;
            p += 4;
        } else if (wire == 1) {
            if (len - p < 8) goto fail;
            p += 8;
        } else {
            goto fail;
        }
    }
    return 0;
fail:
    pbbp2_frame_free(f);
    return -1;
}

int pbbp2_selftest(char *report, size_t report_len)
{
    pbbp2_frame_t f, g;
    uint8_t buf[512];
    int used = 0, fails = 0;

    pbbp2_frame_init(&f);
    f.seq_id = 1234567890123ULL;
    f.log_id = 42;
    f.service = 7;
    f.method = 1;
    pbbp2_header_set(&f, "type", "event");
    pbbp2_header_set(&f, "message_id", "om_abc");
    pbbp2_header_set(&f, "sum", "1");
    pbbp2_header_set(&f, "seq", "0");
    const char *pl = "{\"hello\":\"world\"}";
    f.payload = (uint8_t *)strdup(pl);
    f.payload_len = strlen(pl);

    int n = pbbp2_encode(&f, buf, sizeof(buf));
    int ok = n > 0;
    if (ok) {
        ok = pbbp2_decode(buf, (size_t)n, &g) == 0 &&
             g.seq_id == f.seq_id && g.log_id == 42 && g.service == 7 && g.method == 1 &&
             g.payload_len == f.payload_len && memcmp(g.payload, f.payload, f.payload_len) == 0 &&
             pbbp2_header_get(&g, "message_id") && strcmp(pbbp2_header_get(&g, "message_id"), "om_abc") == 0;
        pbbp2_frame_free(&g);
    }
    if (!ok) fails++;
    int k = snprintf(report + used, report_len - used, "[%s] pbbp2 编解码往返 (%d bytes)\n",
                     ok ? "PASS" : "FAIL", n);
    used += k > 0 ? k : 0;
    pbbp2_frame_free(&f);
    return fails;
}
