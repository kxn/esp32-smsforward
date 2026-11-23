#include "mms_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const mms_profile_t mobile = {"cmwap", "mmsc.monternet.com", "10.0.0.172", 80};
static const mms_profile_t unicom = {"3gwap", "mmsc.myuni.com.cn", "10.0.0.172", 80};
static const mms_profile_t telecom = {"ctwap", "mmsc.vnet.mobi", "10.0.0.200", 80};

static const mms_profile_t *profile_for_plmn(const char *digits)
{
    if (!digits || strncmp(digits, "460", 3)) return NULL;
    switch ((digits[3] - '0') * 10 + digits[4] - '0') {
    case 0: case 2: case 4: case 7: case 8: case 13: return &mobile;
    case 1: case 6: case 9: return &unicom;
    case 3: case 5: case 11: return &telecom;
    default: return NULL;
    }
}

bool mms_serving_home(const mms_profile_t *profile, const char *response)
{
    if (!profile || !response) return false;
    const char *s = strstr(response, "+COPS:");
    if (!s || (s != response && s[-1] != '\n' && s[-1] != '\r') || strstr(s + 6, "+COPS:")) return false;
    int mode = -1, format = -1, end = 0;
    char digits[7] = "";
    if (sscanf(s, "+COPS: %d , %d , \"%6[0123456789]\"%n", &mode, &format, digits, &end) != 3 ||
        !end || (s[end] && !strchr(",\r\n", s[end])) || format != 2 ||
        (mode != 0 && mode != 1 && mode != 4) || strlen(digits) != 5) return false;
    return profile == profile_for_plmn(digits);
}

const mms_profile_t *mms_profile_from_imsi(const char *response)
{
    if (!response) return NULL;
    const char *imsi = NULL;
    for (const char *s = response; *s;) {
        size_t n = strcspn(s, "\r\n");
        if (n == 15 && strspn(s, "0123456789") == n) {
            if (imsi) return NULL;
            imsi = s;
        }
        s += n;
        s += strspn(s, "\r\n");
    }
    return profile_for_plmn(imsi);
}

bool mms_home_registered(const char *response)
{
    if (!response) return false;
    const char *s = strstr(response, "+CEREG:");
    if (!s || (s != response && s[-1] != '\n' && s[-1] != '\r') ||
        strstr(s + 7, "+CEREG:")) return false;
    s += 7;
    while (*s == ' ') ++s;
    if (*s < '0' || *s++ > '5') return false;
    while (*s == ' ') ++s;
    if (*s++ != ',') return false;
    while (*s == ' ') ++s;
    if (*s++ != '1') return false;
    while (*s == ' ') ++s;
    return !*s || *s == ',' || *s == '\r' || *s == '\n';
}

int mms_http_request(const mms_profile_t *p, const char *url, char *out, size_t cap)
{
    if (!p || !url || !out || !cap) return -1;
    const char *path = url;
    if (*path != '/') {
        if (strncasecmp(path, "http://", 7)) return -1;
        path += 7;
        size_t n = strlen(p->host);
        if (strncasecmp(path, p->host, n)) return -1;
        path += n;
        if (!strncmp(path, ":80", 3)) path += 3;
    }
    if (path[0] != '/' || path[1] == '/') return -1;
    for (const unsigned char *s = (const unsigned char *)path; *s; ++s)
        if (*s <= 0x20 || *s >= 0x7f || *s == '\\' || *s == '#') return -1;
    int n = snprintf(out, cap,
        "GET http://%s%s HTTP/1.1\r\nHost: %s\r\nAccept: application/vnd.wap.mms-message\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", p->host, path, p->host);
    return n >= 0 && (size_t)n < cap ? n : -1;
}

static const uint8_t *line_end(const uint8_t *p, const uint8_t *end)
{
    while (end - p >= 2) {
        if (p[0] == '\r' && p[1] == '\n') return p;
        ++p;
    }
    return NULL;
}

static bool equals(const uint8_t *p, size_t n, const char *name)
{
    return n == strlen(name) && !strncasecmp((const char *)p, name, n);
}

static bool number(const uint8_t *s, const uint8_t *end, unsigned base, size_t *out)
{
    if (s == end) return false;
    size_t n = 0;
    for (; s < end; ++s) {
        unsigned digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (base == 16 && tolower(*s) >= 'a' && tolower(*s) <= 'f') digit = tolower(*s) - 'a' + 10;
        else return false;
        if (n > (MMS_BODY_MAX - digit) / base) return false;
        n = n * base + digit;
    }
    *out = n;
    return true;
}

/* Validate before moving bytes so a partial network read can be retried intact. */
static int chunks(uint8_t *wire, uint8_t *p, uint8_t *end, size_t *body_len, bool copy)
{
    size_t count = 0;
    for (;;) {
        const uint8_t *e = line_end(p, end);
        if (!e) return end - p > 512 ? -1 : 0;
        if (e - p > 512) return -1;
        const uint8_t *ext = memchr(p, ';', e - p);
        size_t n;
        if (!number(p, ext ? ext : e, 16, &n) || n > MMS_BODY_MAX - count) return -1;
        p = (uint8_t *)e + 2;
        if (!n) {
            do {
                e = line_end(p, end);
                if (!e) return 0;
                if (e == p) {
                    if (!count || end - p != 2) return -1;
                    *body_len = count;
                    return 1;
                }
                if (e - p > 2048 || !memchr(p, ':', e - p)) return -1;
                p = (uint8_t *)e + 2;
            } while (p < end);
            return 0;
        }
        if ((size_t)(end - p) < n + 2) return 0;
        if (p[n] != '\r' || p[n + 1] != '\n') return -1;
        if (copy) memmove(wire + count, p, n);
        count += n;
        p += n + 2;
    }
}

int mms_http_decode(uint8_t *wire, size_t len, bool eof, size_t *body_len)
{
    if (!wire || !body_len || len > MMS_BODY_MAX + MMS_HTTP_OVERHEAD_MAX) return -1;
    uint8_t *end = wire + len;
    uint8_t *p = wire;
    size_t stated = 0;
    bool chunked = false, have_length = false;
    for (;;) {
        const uint8_t *e = line_end(p, end);
        if (!e) return eof || len >= 8192 ? -1 : 0;
        if (e - wire >= 8192) return -1;
        if (p == wire) {
            if (e - p < 12 || (memcmp(p, "HTTP/1.1 200", 12) && memcmp(p, "HTTP/1.0 200", 12)) ||
                (e - p > 12 && p[12] != ' ')) return -1;
        } else if (e == p) {
            p += 2;
            break;
        } else {
            const uint8_t *k = memchr(p, ':', e - p);
            if (!k || k == p) return -1;
            const uint8_t *v = k + 1, *last = e;
            while (v < last && (*v == ' ' || *v == '\t')) ++v;
            while (last > v && (last[-1] == ' ' || last[-1] == '\t')) --last;
            if (equals(p, k - p, "Content-Length")) {
                if (have_length || chunked || !number(v, last, 10, &stated) || !stated) return -1;
                have_length = true;
            } else if (equals(p, k - p, "Transfer-Encoding")) {
                if (have_length || chunked || !equals(v, last - v, "chunked")) return -1;
                chunked = true;
            } else if (equals(p, k - p, "Content-Encoding") && !equals(v,last-v,"identity")) {
                return -1;
            }
        }
        p = (uint8_t *)e + 2;
    }
    if (chunked) {
        int rc = chunks(wire, p, end, body_len, false);
        if (rc != 1) return eof && rc == 0 ? -1 : rc;
        return chunks(wire, p, end, body_len, true);
    }
    size_t actual = end - p;
    if (actual > MMS_BODY_MAX || (have_length && actual > stated)) return -1;
    if (have_length ? actual < stated : !eof) return eof ? -1 : 0;
    if (!actual) return -1;
    memmove(wire, p, actual);
    *body_len = actual;
    return 1;
}
