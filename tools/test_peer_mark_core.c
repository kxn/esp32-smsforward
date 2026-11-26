#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../firmware/main/peer_mark_core.h"

/* Host regression tests for the pure peer-mark helpers.  Same shape as
 * tools/test_sms_spool.c: one [PASS]/[FAIL] line per assertion, a [SUMMARY]
 * line at the end and a non-zero exit status on any failure. */

static unsigned s_checks;
static int s_failures;

#define CHECK(name, cond) \
    do { \
        ++s_checks; \
        if (cond) { \
            printf("[PASS] %s\n", name); \
        } else { \
            printf("[FAIL] %s\n", name); \
            ++s_failures; \
        } \
    } while (0)

/* ---------------------------------------------------------------- */
/* assertion helpers                                                 */
/* ---------------------------------------------------------------- */

static void expect_url(const char *name, const char *tmpl, const char *key,
                       const char *number, const char *expect)
{
    char out[PEER_MARK_URL_MAX];
    memset(out, 'X', sizeof(out));
    size_t n = peer_mark_build_url(out, sizeof(out), tmpl, key, number);
    bool ok;
    if (expect == NULL) {
        ok = n == 0 && out[0] == '\0';
    } else {
        ok = n == strlen(expect) && strcmp(out, expect) == 0 && out[n + 1] == 'X';
    }
    CHECK(name, ok);
}

static void expect_url_cap(const char *name, const char *tmpl, const char *key,
                           const char *number, size_t cap, const char *expect)
{
    char out[PEER_MARK_URL_MAX];
    memset(out, 'X', sizeof(out));
    size_t n = peer_mark_build_url(out, cap, tmpl, key, number);
    bool ok;
    if (expect == NULL) {
        /* A zero-capacity call cannot be expected to write a terminator. */
        ok = n == 0 && (cap == 0 || out[0] == '\0');
    } else {
        ok = n == strlen(expect) && strcmp(out, expect) == 0 && out[n + 1] == 'X';
    }
    CHECK(name, ok);
}

static void expect_labels_len(const char *name, const char *json, size_t len,
                              const char *expect)
{
    char out[PEER_MARK_LABEL_MAX * PEER_MARK_LABELS_MAX + 2];
    memset(out, 'X', sizeof(out));
    bool got = peer_mark_parse_labels(json, len, out, sizeof(out));
    bool ok;
    if (expect == NULL) {
        ok = !got && out[0] == '\0';
    } else {
        ok = got && strcmp(out, expect) == 0;
    }
    CHECK(name, ok);
}

static void expect_labels(const char *name, const char *json, const char *expect)
{
    expect_labels_len(name, json, strlen(json), expect);
}

static void expect_labels_cap(const char *name, const char *json, size_t cap,
                              const char *expect)
{
    char out[PEER_MARK_LABEL_MAX * PEER_MARK_LABELS_MAX + 2];
    memset(out, 'X', sizeof(out));
    bool got = peer_mark_parse_labels(json, strlen(json), out, cap);
    bool ok;
    if (expect == NULL) {
        ok = !got && (cap == 0 || out[0] == '\0');
    } else {
        ok = got && strcmp(out, expect) == 0;
    }
    CHECK(name, ok);
}

static void expect_join(const char *name, const char *region, const char *mark,
                        const char *expect)
{
    char out[128];
    memset(out, 'X', sizeof(out));
    size_t n = peer_mark_join_label(out, sizeof(out), region, mark);
    bool ok = n == strlen(expect) && strcmp(out, expect) == 0;
    CHECK(name, ok);
}

static void expect_join_cap(const char *name, size_t cap, const char *region,
                            const char *mark, const char *expect)
{
    char out[128];
    memset(out, 'X', sizeof(out));
    size_t n = peer_mark_join_label(out, cap, region, mark);
    bool ok;
    if (cap == 0) {
        ok = n == 0 && out[0] == 'X';
    } else {
        ok = n == strlen(expect) && strcmp(out, expect) == 0;
    }
    CHECK(name, ok);
}

/* ---------------------------------------------------------------- */
/* A. peer_mark_build_url                                            */
/* ---------------------------------------------------------------- */

static void test_build_url_basic(void)
{
    expect_url("url: plain number substitution",
               "http://host/api?phone={number}", NULL, "13800000001",
               "http://host/api?phone=13800000001");
    expect_url("url: both placeholders substituted",
               "https://api.example.com/mark?key={key}&phone={number}", "K1",
               "+8613800000001",
               "https://api.example.com/mark?key=K1&phone=%2B8613800000001");
    expect_url("url: NULL key expands to nothing",
               "https://api.example.com/mark?key={key}&phone={number}", NULL, "1",
               "https://api.example.com/mark?key=&phone=1");
    expect_url("url: empty key expands to nothing",
               "https://api.example.com/mark?key={key}&phone={number}", "", "1",
               "https://api.example.com/mark?key=&phone=1");
    expect_url("url: template with only a number",
               "{number}", NULL, "13800000001", "13800000001");
    expect_url("url: key placeholder absent is not required",
               "http://h/?n={number}", "KEY", "42", "http://h/?n=42");
}

static void test_build_url_encoding(void)
{
    expect_url("url: plus is percent-encoded", "{number}", NULL, "+86", "%2B86");
    expect_url("url: slash colon space percent are encoded", "{number}", NULL,
               "1/2:3 4%5", "1%2F2%3A3%204%255");
    expect_url("url: unreserved characters are kept", "{number}", NULL,
               "aZ09-._~", "aZ09-._~");
    expect_url("url: non-ASCII encoded per byte", "{number}", NULL, "12\xC3\xA9",
               "12%C3%A9");
    expect_url("url: key is percent-encoded too", "?k={key}&n={number}", "a b/c", "9",
               "?k=a%20b%2Fc&n=9");
    expect_url("url: uppercase hex digits", "{number}", NULL, "\x01\x7F",
               "%01%7F");
}

static void test_build_url_braces(void)
{
    expect_url("url: other braces copied literally", "{foo}/{number}/{bar}", NULL, "7",
               "{foo}/7/{bar}");
    expect_url("url: near-miss brace text untouched", "{numbe}/{numberx}/{number}",
               NULL, "7", "{numbe}/{numberx}/7");
    expect_url("url: every {number} is replaced", "{number}-{number}", NULL, "7",
               "7-7");
    expect_url("url: key braces without key config", "{key}/{number}", NULL, "7",
               "/7");
}

static void test_build_url_failures(void)
{
    expect_url("url: template without {number} fails",
               "http://h/?phone={phone}", NULL, "1", NULL);
    expect_url("url: template with only {key} fails", "http://h/?k={key}", "K", "1",
               NULL);
    expect_url("url: empty template fails", "", NULL, "1", NULL);
    expect_url("url: NULL template fails", NULL, NULL, "1", NULL);
    expect_url("url: NULL number fails", "http://h/?n={number}", NULL, NULL, NULL);
    expect_url("url: empty number fails", "http://h/?n={number}", NULL, "", NULL);

    CHECK("url: NULL out fails",
          peer_mark_build_url(NULL, 64, "http://h/?n={number}", NULL, "1") == 0);
    expect_url_cap("url: zero cap fails", "http://h/?n={number}", NULL, "1", 0, NULL);

    /* "http://h/?n=1" is 13 bytes, so it needs cap >= 14. */
    expect_url_cap("url: exact cap fits", "http://h/?n={number}", NULL, "1", 14,
                   "http://h/?n=1");
    expect_url_cap("url: one byte short fails", "http://h/?n={number}", NULL, "1", 13,
                   NULL);
    expect_url_cap("url: tiny cap fails", "http://h/?n={number}", NULL, "1", 1, NULL);
    expect_url_cap("url: expansion counted before writing", "{number}", NULL, "+1", 4,
                   NULL);
    expect_url_cap("url: expansion fits after measuring", "{number}", NULL, "+1", 5,
                   "%2B1");
}

/* ---------------------------------------------------------------- */
/* B. peer_mark_parse_labels                                         */
/* ---------------------------------------------------------------- */

static void test_parse_tags_name(void)
{
    expect_labels("parse: Tmini results[].tags[].name",
                  "{\"results\":[{\"platform\":\"360\",\"tags\":[{\"name\":\"快递\","
                  "\"count\":12}]}]}",
                  "快递");
    expect_labels("parse: data.tags[].name",
                  "{\"data\":{\"tags\":[{\"name\":\"外卖\"}]}}", "外卖");
    expect_labels("parse: two tag names are joined",
                  "{\"tags\":[{\"name\":\"快递\"},{\"name\":\"外卖\"}]}", "快递/外卖");
    expect_labels("parse: tier-one name wins over generic tag",
                  "{\"tags\":[{\"name\":\"快递\"}],\"tag\":\"骚扰\"}", "快递");
    expect_labels("parse: tier-one duplicate collapses with mark",
                  "{\"tags\":[{\"name\":\"快递\"}],\"mark\":\"快递\"}", "快递");
    expect_labels("parse: name outside a tags array is ignored",
                  "{\"name\":\"顺丰\"}", NULL);
    expect_labels("parse: tags as an array of strings",
                  "{\"tags\":[\"骚扰\",\"诈骗\"]}", "骚扰/诈骗");
    expect_labels("parse: three string tags truncate to two",
                  "{\"tags\":[\"a\",\"b\",\"c\"]}", "a/b");
    expect_labels("parse: empty tags array yields nothing", "{\"tags\":[]}", NULL);
    expect_labels("parse: tags as a plain string is not a label list",
                  "{\"tags\":\"快递\"}", NULL);
}

static void test_parse_generic_keys(void)
{
    expect_labels("parse: tag key", "{\"tag\":\"骚扰\"}", "骚扰");
    expect_labels("parse: mark key", "{\"mark\":\"快递\"}", "快递");
    expect_labels("parse: label key", "{\"label\":\"外卖\"}", "外卖");
    expect_labels("parse: category key", "{\"category\":\"广告推销\"}", "广告推销");
    expect_labels("parse: tag nested arbitrarily deep",
                  "{\"a\":{\"b\":{\"label\":\"外卖\"}}}", "外卖");
    expect_labels("parse: duplicate tag and mark collapse",
                  "{\"tag\":\"骚扰\",\"mark\":\"骚扰\"}", "骚扰");
    expect_labels("parse: duplicate string tags collapse",
                  "{\"tag\":\"骚扰\",\"label\":\"骚扰\",\"category\":\"诈骗\"}",
                  "骚扰/诈骗");
    expect_labels("parse: two different generic keys joined",
                  "{\"tag\":\"骚扰\",\"label\":\"诈骗\"}", "骚扰/诈骗");
    expect_labels("parse: whitespace in the body tolerated",
                  " \n{\r\n \"tag\" : \"骚扰\" \t}\n ", "骚扰");
}

static void test_parse_juhe(void)
{
    static const struct {
        int fid;
        const char *label;
    } cases[] = {
        {1, "骚扰电话"}, {2, "推销"},       {3, "中介"},     {4, "快递送餐"},
        {5, "疑似诈骗"}, {6, "招聘猎头"},   {7, "出租车司机"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char json[128];
        char name[80];
        snprintf(json, sizeof(json),
                 "{\"result\":{\"flag\":{\"fid\":%d,\"num\":\"12\"}}}", cases[i].fid);
        snprintf(name, sizeof(name), "parse: JUHE fid %d maps to %s", cases[i].fid,
                 cases[i].label);
        expect_labels(name, json, cases[i].label);
    }
    expect_labels("parse: JUHE-shaped body",
                  "{\"result\":{\"flag\":{\"fid\":4,\"num\":\"12\"},"
                  "\"catnames\":\"快递送餐\",\"name\":\"顺丰\"}}",
                  "快递送餐");
    expect_labels("parse: unknown fid is ignored", "{\"result\":{\"flag\":{\"fid\":8}}}",
                  NULL);
    expect_labels("parse: fid zero is ignored", "{\"fid\":0}", NULL);
    expect_labels("parse: fractional fid is not an id",
                  "{\"fid\":4.5,\"tag\":\"外卖\"}", "外卖");
    expect_labels("parse: negative fid is ignored", "{\"fid\":-1}", NULL);
    expect_labels("parse: fid as a string is ignored", "{\"fid\":\"4\"}", NULL);
}

static void test_parse_catnames(void)
{
    expect_labels("parse: catnames splits on ASCII comma",
                  "{\"catnames\":\"快递送餐,外卖\"}", "快递送餐/外卖");
    expect_labels("parse: catnames splits on fullwidth comma",
                  "{\"catnames\":\"快递送餐，外卖\"}", "快递送餐/外卖");
    expect_labels("parse: catnames splits on slash",
                  "{\"catnames\":\"快递送餐/外卖\"}", "快递送餐/外卖");
    expect_labels("parse: catnames keeps only two",
                  "{\"catnames\":\"a,b,c\"}", "a/b");
    expect_labels("parse: catnames skips empty pieces",
                  "{\"catnames\":\",,快递,\"}", "快递");
    expect_labels("parse: catnames pieces are trimmed",
                  "{\"catnames\":\" 快递 , 外卖 \"}", "快递/外卖");
    expect_labels("parse: catname singular accepted", "{\"catname\":\"中介\"}", "中介");
    expect_labels("parse: catnames and fid agree without duplicating",
                  "{\"fid\":4,\"catnames\":\"快递送餐,外卖\"}", "快递送餐/外卖");
}

static void test_parse_placeholders(void)
{
    static const char *const ph[] = {
        "",       "未标记", "无标记", "无",     "未知",   "正常",   "普通",
        "normal", "none",   "null",   "unknown", "no mark", "NORMAL", " 未标记 ",
    };
    for (size_t i = 0; i < sizeof(ph) / sizeof(ph[0]); ++i) {
        char json[128];
        char name[96];
        snprintf(json, sizeof(json), "{\"tag\":\"%s\"}", ph[i]);
        snprintf(name, sizeof(name), "parse: placeholder \"%s\" ignored",
                 ph[i][0] != '\0' ? ph[i] : "<empty>");
        expect_labels(name, json, NULL);
    }
}

static void test_parse_malformed(void)
{
    expect_labels("parse: lone brace is malformed", "{", NULL);
    expect_labels("parse: lone bracket is malformed", "[", NULL);
    expect_labels("parse: bare key is malformed", "\"name\":", NULL);
    expect_labels("parse: empty body yields nothing", "", NULL);
    expect_labels_len("parse: zero length yields nothing", "{\"tag\":\"骚扰\"}", 0,
                      NULL);
    CHECK("parse: NULL json fails",
          !peer_mark_parse_labels(NULL, 10, (char[8]){0}, 8));
    CHECK("parse: NULL out fails",
          !peer_mark_parse_labels("{\"tag\":\"a\"}", 12, NULL, 8));
    expect_labels_cap("parse: zero cap fails", "{\"tag\":\"a\"}", 0, NULL);
    expect_labels("parse: trailing garbage after a complete label keeps it",
                  "{\"tag\":\"骚扰\"} extra", "骚扰");
    expect_labels("parse: mismatched close keeps the collected label",
                  "{\"tag\":\"骚扰\"]", "骚扰");
}

static void test_parse_truncation(void)
{
    const char *body = "{\"tag\":\"骚扰\"}";
    size_t full = strlen(body);
    expect_labels_len("parse: complete body yields the tag", body, full, "骚扰");
    expect_labels_len("parse: missing final brace still yields the tag", body,
                      full - 1, "骚扰");
    expect_labels_len("parse: truncated after a complete tag still yields it",
                      "{\"tag\":\"骚扰\",\"x\":", strlen("{\"tag\":\"骚扰\",\"x\":"),
                      "骚扰");
    expect_labels_len("parse: truncated inside a tag name object",
                      "{\"tags\":[{\"name\":\"快递\"}",
                      strlen("{\"tags\":[{\"name\":\"快递\"}"), "快递");
    expect_labels_len("parse: cut before the closing quote yields nothing", body, 8,
                      NULL);
    expect_labels_len("parse: cut inside a UTF-8 sequence yields nothing", body, 10,
                      NULL);

    static const char with_nul[] = {'{', '"', 't', 'a', 'g', '"', ':',
                                    '"', 'a', '\0', 'b', '"', '}'};
    expect_labels_len("parse: NUL byte inside a label is skipped", with_nul,
                      sizeof(with_nul), NULL);
}

static void test_parse_escapes(void)
{
    expect_labels("parse: escaped quote decoded", "{\"tag\":\"a\\\"b\"}", "a\"b");
    expect_labels("parse: escaped backslash decoded", "{\"tag\":\"a\\\\b\"}", "a\\b");
    expect_labels("parse: escaped slash decoded", "{\"tag\":\"a\\/b\"}", "a/b");
    expect_labels("parse: escaped newline decoded", "{\"tag\":\"a\\nb\"}", "a\nb");
    expect_labels("parse: escaped tab decoded", "{\"tag\":\"a\\tb\"}", "a\tb");
    expect_labels("parse: escaped carriage return decoded", "{\"tag\":\"a\\rb\"}",
                  "a\rb");
    expect_labels("parse: escaped backspace decoded", "{\"tag\":\"a\\bb\"}", "a\bb");
    expect_labels("parse: escaped formfeed decoded", "{\"tag\":\"a\\fb\"}", "a\fb");
    /* Providers that return Chinese as \uXXXX escapes must still get labels. */
    expect_labels("parse: unicode escape decodes to the label",
                  "{\"tag\":\"\\u9a9a\\u6270\"}", "骚扰");
    expect_labels("parse: unicode escape in one label only",
                  "{\"tag\":\"\\u9a9a\\u6270\",\"mark\":\"快递\"}", "骚扰/快递");
    expect_labels("parse: ascii unicode escape", "{\"tag\":\"\\u0041\\u0042\"}", "AB");
    expect_labels("parse: surrogate pair becomes one code point",
                  "{\"tag\":\"\\ud83d\\ude00\"}", "\xF0\x9F\x98\x80");
    expect_labels("parse: lone high surrogate is dropped", "{\"tag\":\"\\ud83d\"}", NULL);
    expect_labels("parse: lone low surrogate is dropped", "{\"tag\":\"\\ude00\"}", NULL);
    expect_labels("parse: nul escape is dropped", "{\"tag\":\"\\u0000\"}", NULL);
    expect_labels("parse: escaped quote inside label", "{\"tag\":\"a\\\"b\"}", "a\"b");
    expect_labels("parse: invalid escape skips the label",
                  "{\"tag\":\"a\\qb\"}", NULL);
    expect_labels("parse: truncated unicode escape is malformed",
                  "{\"tag\":\"a\\u62\"}", NULL);
}

static void test_parse_label_lengths(void)
{
    char unit3[4] = "\xE5\xBF\xAB"; /* U+5FEB 快 */
    char label47[64];
    char label48[64];
    char json[192];
    size_t k = 0;

    for (int i = 0; i < 15; ++i) {
        memcpy(label47 + k, unit3, 3);
        k += 3;
    }
    memcpy(label47 + k, "ab", 2);
    k += 2;
    label47[k] = '\0';
    CHECK("parse: 47-byte label fixture is 47 bytes", strlen(label47) == 47);

    snprintf(json, sizeof(json), "{\"tag\":\"%s\"}", label47);
    expect_labels("parse: 47-byte label accepted", json, label47);
    expect_labels_cap("parse: 47-byte label fits a 48-byte buffer", json, 48, label47);
    expect_labels_cap("parse: 47-byte label needs room for the NUL", json, 47, NULL);

    k = 0;
    for (int i = 0; i < 16; ++i) {
        memcpy(label48 + k, unit3, 3);
        k += 3;
    }
    label48[k] = '\0';
    CHECK("parse: 48-byte label fixture is 48 bytes", strlen(label48) == 48);
    snprintf(json, sizeof(json), "{\"tag\":\"%s\",\"mark\":\"快递\"}", label48);
    expect_labels("parse: 48-byte label skipped, not truncated", json, "快递");

    expect_labels("parse: stray continuation byte skipped",
                  "{\"tag\":\"\x80\"}", NULL);
    expect_labels("parse: truncated UTF-8 label skipped",
                  "{\"tag\":\"\xE9\xA9\"}", NULL);
}

static void test_parse_capacity_and_depth(void)
{
    expect_labels_cap("parse: exact cap fits", "{\"tag\":\"abc\"}", 4, "abc");
    expect_labels_cap("parse: one byte short refused", "{\"tag\":\"abc\"}", 3, NULL);
    expect_labels_cap("parse: first label too big for cap", "{\"tags\":[\"aaa\"]}", 3,
                      NULL);
    expect_labels_cap("parse: trailing label dropped when cap is tight",
                      "{\"tags\":[\"aaa\",\"bbb\"]}", 4, "aaa");
    expect_labels_cap("parse: second label fits exactly", "{\"tags\":[\"aaa\",\"bbb\"]}",
                      8, "aaa/bbb");
    expect_labels_cap("parse: second label dropped one byte short",
                      "{\"tags\":[\"aaa\",\"bbb\"]}", 7, "aaa");

    {
        char json[256];
        const char *inner = "{\"tag\":\"外卖\"}";
        size_t inner_len = strlen(inner);
        size_t pos = 0;
        for (int i = 0; i < 20; ++i) json[pos++] = '[';
        memcpy(json + pos, inner, inner_len);
        pos += inner_len;
        for (int i = 0; i < 20; ++i) json[pos++] = ']';
        json[pos] = '\0';
        expect_labels("parse: 20 nested arrays still scanned", json, "外卖");

        pos = 0;
        for (int i = 0; i < 40; ++i) json[pos++] = '[';
        memcpy(json + pos, inner, inner_len);
        pos += inner_len;
        for (int i = 0; i < 40; ++i) json[pos++] = ']';
        json[pos] = '\0';
        expect_labels("parse: nesting beyond the depth limit rejected", json, NULL);
    }
}

/* ---------------------------------------------------------------- */
/* C. peer_mark_join_label                                           */
/* ---------------------------------------------------------------- */

static void test_join_label(void)
{
    expect_join("join: both parts", "北京", "快递/外卖", "北京 \xC2\xB7 快递/外卖");
    expect_join("join: only region", "北京", NULL, "北京");
    expect_join("join: only region with empty mark", "北京", "", "北京");
    expect_join("join: only mark", NULL, "骚扰", "骚扰");
    expect_join("join: only mark with empty region", "", "骚扰", "骚扰");
    expect_join("join: neither part", NULL, NULL, "");
    expect_join("join: both empty strings", "", "", "");
    expect_join("join: middle dot is two bytes", "a", "b", "a \xC2\xB7 b");

    expect_join_cap("join: exact fit", 24, "北京", "快递/外卖",
                    "北京 \xC2\xB7 快递/外卖");
    expect_join_cap("join: byte before the dot keeps the region", 8, "北京", "外卖",
                    "北京 ");
    expect_join_cap("join: dot split trimmed back", 9, "北京", "外卖", "北京 ");
    expect_join_cap("join: truncation keeps complete characters only", 10, "北京",
                    "外卖", "北京 \xC2\xB7");
    expect_join_cap("join: one spare byte still keeps both parts", 17, "北京", "外卖",
                    "北京 \xC2\xB7 外卖");
    expect_join_cap("join: mark truncated at a UTF-8 boundary", 16, "北京", "外卖",
                    "北京 \xC2\xB7 外");
    expect_join_cap("join: three-part mark truncated at a boundary", 16, "北京",
                    "快递/外卖", "北京 \xC2\xB7 快");
    expect_join_cap("join: region-only truncation at a boundary", 4, "北京", NULL, "北");
    expect_join_cap("join: region-only cannot fit one character", 3, "北京", NULL, "");
    expect_join_cap("join: mark-only truncation at a boundary", 4, NULL, "外卖", "外");
    expect_join_cap("join: cap of one byte yields empty", 1, "北京", "外卖", "");
    CHECK("join: NULL out fails",
          peer_mark_join_label(NULL, 8, "北京", "外卖") == 0);
    {
        char out[8];
        memset(out, 'X', sizeof(out));
        CHECK("join: zero cap leaves the buffer alone",
              peer_mark_join_label(out, 0, "北京", NULL) == 0 && out[0] == 'X');
    }
}

static void test_normalise_number(void)
{
    /* ---- canonical query number ---- */
{
    char out[PEER_MARK_NUMBER_MAX];
    memset(out, 'X', sizeof(out));
    CHECK("number: plain digits kept",
          peer_mark_normalise_number("13800138000", out, sizeof(out)) == 11 &&
          strcmp(out, "13800138000") == 0);
    CHECK("number: separators dropped",
          peer_mark_normalise_number("010-2128 5387", out, sizeof(out)) == 11 &&
          strcmp(out, "01021285387") == 0);
    CHECK("number: plus and country code dropped",
          peer_mark_normalise_number("+8613800138000", out, sizeof(out)) == 11 &&
          strcmp(out, "13800138000") == 0);
    CHECK("number: 0086 prefix dropped",
          peer_mark_normalise_number("008613800138000", out, sizeof(out)) == 11 &&
          strcmp(out, "13800138000") == 0);
    CHECK("number: bare 86 prefix dropped",
          peer_mark_normalise_number("8613800138000", out, sizeof(out)) == 11 &&
          strcmp(out, "13800138000") == 0);
    CHECK("number: foreign number keeps its digits",
          peer_mark_normalise_number("+14155551234", out, sizeof(out)) == 11 &&
          strcmp(out, "14155551234") == 0);
    CHECK("number: 86 after a non-Chinese prefix is kept",
          peer_mark_normalise_number("8613800138", out, sizeof(out)) == 10 &&
          strcmp(out, "8613800138") == 0);
    CHECK("number: no digits fails",
          peer_mark_normalise_number("+", out, sizeof(out)) == 0);
    CHECK("number: NULL input fails",
          peer_mark_normalise_number(NULL, out, sizeof(out)) == 0);
    CHECK("number: tiny cap fails",
          peer_mark_normalise_number("13800138000", out, 4) == 0);
    CHECK("number: exact cap fits",
          peer_mark_normalise_number("110", out, 4) == 3 && strcmp(out, "110") == 0);
}
}

int main(void)
{
    test_build_url_basic();
    test_build_url_encoding();
    test_build_url_braces();
    test_build_url_failures();
    test_parse_tags_name();
    test_parse_generic_keys();
    test_parse_juhe();
    test_parse_catnames();
    test_parse_placeholders();
    test_parse_malformed();
    test_parse_truncation();
    test_parse_escapes();
    test_parse_label_lengths();
    test_parse_capacity_and_depth();
    test_join_label();
    test_normalise_number();

    if (s_failures) {
        printf("[FAIL] peer mark core tests\n");
    printf("[SUMMARY] %u checks, %d failed\n", s_checks, s_failures);
        return 1;
    }
    printf("[PASS] peer mark core tests\n");
    printf("[SUMMARY] %u checks, %d failed\n", s_checks, s_failures);
    return 0;
}
