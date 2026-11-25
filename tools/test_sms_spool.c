#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../firmware/main/sms_spool_core.h"

/* Worst case blob: header plus 64 entries with every field at capacity. */
#define MAX_BLOB ((560u * 1024u))

static sms_spool_store_t g_store;
static sms_spool_store_t g_other;
static sms_spool_store_t g_scratch;
static uint8_t g_blob[MAX_BLOB];
static uint8_t g_blob2[MAX_BLOB];
static char g_big_text[SMS_SPOOL_TEXT_MAX + 1];

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

static void set_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void make_entry(sms_spool_entry_t *e, uint64_t fp, uint8_t kind, uint8_t slot,
                       const char *peer, const char *text)
{
    char id[32];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->slot = slot;
    e->ts_valid = 1;
    e->durable = 0;
    e->received_unix = (int64_t)(UINT64_C(1700000000) + (fp % 1000));
    e->fingerprint = fp;
    set_str(e->peer, sizeof(e->peer), peer);
    snprintf(id, sizeof(id), "rec_%llu", (unsigned long long)fp);
    set_str(e->record_id, sizeof(e->record_id), id);
    snprintf(id, sizeof(id), "om_%llu", (unsigned long long)fp);
    set_str(e->msg_id, sizeof(e->msg_id), id);
    set_str(e->text, sizeof(e->text), text);
    e->text_len = (uint16_t)strlen(e->text);
}

static bool entry_equal(const sms_spool_entry_t *a, const sms_spool_entry_t *b)
{
    if (a->kind != b->kind || a->slot != b->slot || a->ts_valid != b->ts_valid ||
        a->durable != b->durable || a->received_unix != b->received_unix ||
        a->fingerprint != b->fingerprint || a->text_len != b->text_len) {
        return false;
    }
    if (strcmp(a->peer, b->peer) != 0 || strcmp(a->record_id, b->record_id) != 0 ||
        strcmp(a->msg_id, b->msg_id) != 0) {
        return false;
    }
    return memcmp(a->text, b->text, (size_t)a->text_len + 1) == 0;
}

/* --- helpers that mirror the private blob format for corruption tests --- */

static uint32_t test_fnv(uint32_t hash, const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        hash ^= p[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static uint32_t test_checksum(const uint8_t *buf, size_t len)
{
    uint32_t hash = UINT32_C(2166136261);
    hash = test_fnv(hash, buf, 12);
    return test_fnv(hash, buf + 16, len - 16);
}

static void test_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void test_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void reseal(uint8_t *buf, size_t len)
{
    test_put_u32(buf + 12, test_checksum(buf, len));
}

static void expect_corrupt(const uint8_t *buf, size_t len, const char *name)
{
    bool corrupt = false;
    sms_spool_init(&g_scratch);
    unsigned got = sms_spool_decode(&g_scratch, buf, len, &corrupt);
    CHECK(name, got == 0 && corrupt && sms_spool_count(&g_scratch) == 0);
}

static void test_empty_store(void)
{
    sms_spool_init(&g_store);
    CHECK("empty: count is zero", sms_spool_count(&g_store) == 0);
    CHECK("empty: not full", !sms_spool_is_full(&g_store));
    CHECK("empty: at(0) is null", sms_spool_at(&g_store, 0) == NULL);
    CHECK("empty: find misses", sms_spool_find(&g_store, 1) == NULL);
    CHECK("empty: all durable", sms_spool_all_durable(&g_store));

    size_t len = sms_spool_encode(&g_store, g_blob, sizeof(g_blob));
    CHECK("empty: encode is header only", len == 16);
    bool corrupt = true;
    sms_spool_init(&g_other);
    unsigned got = sms_spool_decode(&g_other, g_blob, len, &corrupt);
    CHECK("empty: decode round trip", got == 0 && !corrupt && sms_spool_count(&g_other) == 0);
}

static void test_add_order_dedup(void)
{
    sms_spool_entry_t e;
    sms_spool_init(&g_store);
    make_entry(&e, 101, SMS_SPOOL_KIND_SMS, 0, "+8613800000001", "hello one");
    sms_spool_entry_t *p1 = sms_spool_add(&g_store, &e);
    CHECK("add: returns entry", p1 != NULL && p1->fingerprint == 101);
    make_entry(&e, 102, SMS_SPOOL_KIND_CALL, 1, "+8613800000002", "missed call");
    sms_spool_add(&g_store, &e);
    make_entry(&e, 103, SMS_SPOOL_KIND_SMS, 1, "+8613800000003", "third");
    sms_spool_add(&g_store, &e);
    CHECK("add: appended three", sms_spool_count(&g_store) == 3 && g_store.added == 3 &&
                                 g_store.dropped == 0);
    CHECK("add: index 0 is oldest",
          sms_spool_at(&g_store, 0) && sms_spool_at(&g_store, 0)->fingerprint == 101);
    CHECK("add: index 1 is middle",
          sms_spool_at(&g_store, 1) && sms_spool_at(&g_store, 1)->fingerprint == 102);
    CHECK("add: index 2 is newest",
          sms_spool_at(&g_store, 2) && sms_spool_at(&g_store, 2)->fingerprint == 103);
    CHECK("add: at(3) is null", sms_spool_at(&g_store, 3) == NULL);
    CHECK("add: find locates entry", sms_spool_find(&g_store, 102) == sms_spool_at(&g_store, 1));

    /* A repeated fingerprint keeps its position but refreshes its payload. */
    make_entry(&e, 102, SMS_SPOOL_KIND_CALL, 1, "+8613800000002", "call updated text");
    e.durable = 1;
    e.received_unix = 1700123456;
    sms_spool_entry_t *dup = sms_spool_add(&g_store, &e);
    CHECK("dedup: no append", sms_spool_count(&g_store) == 3 && g_store.added == 3 &&
                              dup == sms_spool_find(&g_store, 102));
    CHECK("dedup: refreshes text",
          dup && dup->text_len == strlen("call updated text") &&
          strcmp(dup->text, "call updated text") == 0);
    CHECK("dedup: refreshes time and durable",
          dup && dup->received_unix == 1700123456 && dup->durable == 1);
    CHECK("dedup: keeps kind and slot",
          dup && dup->kind == SMS_SPOOL_KIND_CALL && dup->slot == 1);
}

static void test_full_and_remove(void)
{
    sms_spool_entry_t e;
    for (uint64_t fp = 200; sms_spool_count(&g_store) < SMS_SPOOL_CAPACITY; ++fp) {
        char text[32];
        snprintf(text, sizeof(text), "fill %llu", (unsigned long long)fp);
        make_entry(&e, fp, SMS_SPOOL_KIND_SMS, (uint8_t)(fp & 1), "+8613800000999", text);
        sms_spool_add(&g_store, &e);
    }
    CHECK("full: capacity reached", sms_spool_count(&g_store) == SMS_SPOOL_CAPACITY);
    CHECK("full: is_full reports it", sms_spool_is_full(&g_store));
    uint32_t added = g_store.added;
    uint32_t dropped = g_store.dropped;
    make_entry(&e, 9999, SMS_SPOOL_KIND_SMS, 0, "+8613800009999", "overflow");
    CHECK("full: add returns null", sms_spool_add(&g_store, &e) == NULL);
    CHECK("full: dropped counted",
          g_store.dropped == dropped + 1 && g_store.added == added &&
          sms_spool_count(&g_store) == SMS_SPOOL_CAPACITY);

    CHECK("remove: missing fingerprint", !sms_spool_remove(&g_store, 123456));
    CHECK("remove: middle keeps order",
          sms_spool_remove(&g_store, 103) && sms_spool_count(&g_store) == SMS_SPOOL_CAPACITY - 1 &&
          sms_spool_at(&g_store, 2) && sms_spool_at(&g_store, 2)->fingerprint == 200);
    CHECK("remove: oldest shifts down",
          sms_spool_remove(&g_store, 101) && sms_spool_at(&g_store, 0) &&
          sms_spool_at(&g_store, 0)->fingerprint == 102);
    unsigned count = sms_spool_count(&g_store);
    make_entry(&e, 500, SMS_SPOOL_KIND_SMS, 0, "+8613800000777", "after remove");
    CHECK("remove: add appends at tail",
          sms_spool_add(&g_store, &e) && sms_spool_count(&g_store) == count + 1 &&
          sms_spool_at(&g_store, count) && sms_spool_at(&g_store, count)->fingerprint == 500);
}

static void test_durability(void)
{
    sms_spool_entry_t e;
    sms_spool_init(&g_scratch);
    make_entry(&e, 301, SMS_SPOOL_KIND_SMS, 0, "+8613800000011", "a");
    sms_spool_add(&g_scratch, &e);
    make_entry(&e, 302, SMS_SPOOL_KIND_SMS, 0, "+8613800000012", "b");
    sms_spool_add(&g_scratch, &e);
    CHECK("durable: none yet", !sms_spool_all_durable(&g_scratch));
    CHECK("durable: missing fingerprint", !sms_spool_mark_durable(&g_scratch, 789, true));
    CHECK("durable: one marked",
          sms_spool_mark_durable(&g_scratch, 301, true) &&
          sms_spool_find(&g_scratch, 301) && sms_spool_find(&g_scratch, 301)->durable &&
          !sms_spool_all_durable(&g_scratch));
    CHECK("durable: all marked",
          sms_spool_mark_durable(&g_scratch, 302, true) && sms_spool_all_durable(&g_scratch));
    CHECK("durable: clearing one",
          sms_spool_mark_durable(&g_scratch, 301, false) && !sms_spool_all_durable(&g_scratch));
    sms_spool_init(&g_scratch);
    CHECK("durable: empty store is vacuously durable", sms_spool_all_durable(&g_scratch));
}

static size_t build_roundtrip_store(void)
{
    sms_spool_entry_t e;
    for (size_t i = 0; i < SMS_SPOOL_TEXT_MAX; ++i) g_big_text[i] = (char)('A' + (i % 26));
    g_big_text[SMS_SPOOL_TEXT_MAX] = '\0';

    sms_spool_init(&g_other);
    make_entry(&e, 1001, SMS_SPOOL_KIND_SMS, 0, "+8613800000001", "");
    sms_spool_add(&g_other, &e);
    make_entry(&e, 1002, SMS_SPOOL_KIND_CALL, 1, "+8613800000002", g_big_text);
    e.ts_valid = 0;
    e.received_unix = -123456789;
    sms_spool_add(&g_other, &e);
    make_entry(&e, 1003, SMS_SPOOL_KIND_SMS, 1, "+8613800000003",
               "multipart tail with UTF-8 text");
    e.durable = 1;
    e.received_unix = INT64_MAX;
    sms_spool_add(&g_other, &e);
    make_entry(&e, 1004, SMS_SPOOL_KIND_CALL, 0, "+8613800000004", "x");
    memset(e.peer, 'P', sizeof(e.peer) - 1);
    e.peer[sizeof(e.peer) - 1] = '\0';
    memset(e.record_id, 'R', sizeof(e.record_id) - 1);
    e.record_id[sizeof(e.record_id) - 1] = '\0';
    memset(e.msg_id, 'M', sizeof(e.msg_id) - 1);
    e.msg_id[sizeof(e.msg_id) - 1] = '\0';
    sms_spool_add(&g_other, &e);
    return sms_spool_encode(&g_other, g_blob, sizeof(g_blob));
}

static void test_roundtrip(void)
{
    size_t blob_len = build_roundtrip_store();
    CHECK("roundtrip: encode wrote blob",
          blob_len > 16 + SMS_SPOOL_TEXT_MAX && blob_len < sizeof(g_blob));

    sms_spool_init(&g_store);
    bool corrupt = true;
    unsigned got = sms_spool_decode(&g_store, g_blob, blob_len, &corrupt);
    CHECK("roundtrip: decode restored all",
          got == sms_spool_count(&g_other) && !corrupt &&
          sms_spool_count(&g_store) == sms_spool_count(&g_other));

    bool same = true;
    for (unsigned i = 0; same && i < sms_spool_count(&g_other); ++i) {
        const sms_spool_entry_t *a = sms_spool_at(&g_other, i);
        const sms_spool_entry_t *b = sms_spool_at(&g_store, i);
        same = a && b && entry_equal(a, b);
    }
    CHECK("roundtrip: every field preserved", same);
    CHECK("roundtrip: 8 KB text intact",
          sms_spool_at(&g_store, 1) &&
          sms_spool_at(&g_store, 1)->text_len == SMS_SPOOL_TEXT_MAX &&
          strcmp(sms_spool_at(&g_store, 1)->text, g_big_text) == 0);

    size_t again = sms_spool_encode(&g_store, g_blob2, sizeof(g_blob2));
    CHECK("roundtrip: encode is deterministic",
          again == blob_len && memcmp(g_blob, g_blob2, blob_len) == 0);

    /* Decoding appends after events that arrived while the blob was on flash. */
    sms_spool_entry_t e;
    sms_spool_init(&g_scratch);
    make_entry(&e, 2001, SMS_SPOOL_KIND_SMS, 0, "+8613800000201", "local event");
    sms_spool_add(&g_scratch, &e);
    corrupt = true;
    got = sms_spool_decode(&g_scratch, g_blob, blob_len, &corrupt);
    CHECK("decode: appends to a live store",
          got == sms_spool_count(&g_other) && !corrupt &&
          sms_spool_count(&g_scratch) == sms_spool_count(&g_other) + 1);
    CHECK("decode: appended entries keep order",
          sms_spool_at(&g_scratch, 0) && sms_spool_at(&g_scratch, 0)->fingerprint == 2001 &&
          sms_spool_at(&g_scratch, 1) && sms_spool_at(&g_scratch, 1)->fingerprint == 1001);

    corrupt = true;
    got = sms_spool_decode(&g_scratch, g_blob, blob_len, &corrupt);
    CHECK("decode: replaying a blob does not duplicate",
          got == 0 && !corrupt && sms_spool_count(&g_scratch) == sms_spool_count(&g_other) + 1);
    CHECK("decode: null corrupt flag accepted",
          sms_spool_decode(&g_scratch, g_blob, blob_len, NULL) == 0);
}

static void test_corruption(void)
{
    size_t blob_len = sms_spool_encode(&g_other, g_blob, sizeof(g_blob));
    CHECK("corrupt: fixture blob available", blob_len > 16 + SMS_SPOOL_TEXT_MAX);

    memcpy(g_blob2, g_blob, blob_len);
    g_blob2[0] = 'X';
    expect_corrupt(g_blob2, blob_len, "corrupt: bad magic rejected");

    memcpy(g_blob2, g_blob, blob_len);
    g_blob2[4] = 2;
    expect_corrupt(g_blob2, blob_len, "corrupt: bad version rejected");

    memcpy(g_blob2, g_blob, blob_len);
    g_blob2[blob_len - 1] ^= 0xffu;
    expect_corrupt(g_blob2, blob_len, "corrupt: bad checksum rejected");

    memcpy(g_blob2, g_blob, blob_len);
    test_put_u32(g_blob2 + 8, (uint32_t)(blob_len - 16 - 1));
    expect_corrupt(g_blob2, blob_len, "corrupt: payload length mismatch rejected");

    expect_corrupt(g_blob, 8, "corrupt: truncated header rejected");
    expect_corrupt(g_blob, 0, "corrupt: empty buffer rejected");
    expect_corrupt(g_blob, 16, "corrupt: header without payload rejected");
    expect_corrupt(NULL, 0, "corrupt: null buffer rejected");
    expect_corrupt(g_blob, blob_len + 1, "corrupt: trailing garbage rejected");

    /* Oversized entry text length, with a valid checksum so the length check
     * itself is what rejects the blob. */
    memcpy(g_blob2, g_blob, blob_len);
    test_put_u16(g_blob2 + 16 + 20, (uint16_t)(SMS_SPOOL_TEXT_MAX + 1));
    reseal(g_blob2, blob_len);
    expect_corrupt(g_blob2, blob_len, "corrupt: oversized text_len rejected");

    memcpy(g_blob2, g_blob, blob_len);
    g_blob2[16 + 22] = (uint8_t)SMS_SPOOL_PEER_MAX;
    reseal(g_blob2, blob_len);
    expect_corrupt(g_blob2, blob_len, "corrupt: unterminated peer length rejected");

    memcpy(g_blob2, g_blob, blob_len);
    test_put_u16(g_blob2 + 6, (uint16_t)(SMS_SPOOL_CAPACITY + 1));
    reseal(g_blob2, blob_len);
    expect_corrupt(g_blob2, blob_len, "corrupt: count above capacity rejected");

    memcpy(g_blob2, g_blob, blob_len);
    test_put_u16(g_blob2 + 6, (uint16_t)(sms_spool_count(&g_other) - 1));
    reseal(g_blob2, blob_len);
    expect_corrupt(g_blob2, blob_len, "corrupt: count below payload rejected");

    memcpy(g_blob2, g_blob, blob_len);
    test_put_u16(g_blob2 + 6, (uint16_t)(sms_spool_count(&g_other) + 1));
    reseal(g_blob2, blob_len);
    expect_corrupt(g_blob2, blob_len, "corrupt: count above payload rejected");

    /* A single-entry blob whose last text byte is cut off but whose header is
     * internally consistent: the entry length check must reject it. */
    sms_spool_entry_t e;
    make_entry(&e, 4001, SMS_SPOOL_KIND_SMS, 0, "+8613800000401", "truncated entry text");
    sms_spool_init(&g_scratch);
    sms_spool_add(&g_scratch, &e);
    size_t one_len = sms_spool_encode(&g_scratch, g_blob2, sizeof(g_blob2));
    test_put_u32(g_blob2 + 8, (uint32_t)(one_len - 16 - 1));
    reseal(g_blob2, one_len - 1);
    expect_corrupt(g_blob2, one_len - 1, "corrupt: truncated entry rejected");

    /* Corruption never disturbs events already waiting in the store. */
    sms_spool_init(&g_scratch);
    make_entry(&e, 3001, SMS_SPOOL_KIND_SMS, 0, "+8613800000301", "already queued");
    sms_spool_add(&g_scratch, &e);
    memcpy(g_blob2, g_blob, blob_len);
    g_blob2[blob_len - 1] ^= 0xffu;
    bool corrupt = false;
    unsigned got = sms_spool_decode(&g_scratch, g_blob2, blob_len, &corrupt);
    CHECK("corrupt: existing store untouched",
          got == 0 && corrupt && sms_spool_count(&g_scratch) == 1 &&
          sms_spool_at(&g_scratch, 0) && sms_spool_at(&g_scratch, 0)->fingerprint == 3001);
}

static void test_encode_capacity(void)
{
    size_t blob_len = sms_spool_encode(&g_other, g_blob, sizeof(g_blob));
    CHECK("encode: exact fit",
          blob_len > 16 && sms_spool_encode(&g_other, g_blob2, blob_len) == blob_len &&
          memcmp(g_blob2, g_blob, blob_len) == 0);
    CHECK("encode: one byte short refused",
          sms_spool_encode(&g_other, g_blob2, blob_len - 1) == 0 &&
          memcmp(g_blob2, g_blob, blob_len) == 0);
    CHECK("encode: tiny buffer refused",
          sms_spool_encode(&g_other, g_blob2, 16) == 0 &&
          memcmp(g_blob2, g_blob, blob_len) == 0);
    CHECK("encode: null buffer refused", sms_spool_encode(&g_other, NULL, MAX_BLOB) == 0);

    sms_spool_init(&g_scratch);
    CHECK("encode: empty exact fit", sms_spool_encode(&g_scratch, g_blob2, 16) == 16);
    CHECK("encode: empty one byte short", sms_spool_encode(&g_scratch, g_blob2, 15) == 0);
}

static void test_defensive_input(void)
{
    sms_spool_entry_t e;
    sms_spool_init(&g_scratch);

    /* Untrusted caller input must be clamped and terminated, never copied past
     * the declared fields. */
    memset(&e, 0, sizeof(e));
    e.fingerprint = 7001;
    e.kind = SMS_SPOOL_KIND_SMS;
    memset(e.peer, 'Q', sizeof(e.peer));
    memset(e.text, 'Z', sizeof(e.text));
    e.text_len = 60000;
    sms_spool_entry_t *norm = sms_spool_add(&g_scratch, &e);
    CHECK("defensive: text length clamped",
          norm && norm->text_len == SMS_SPOOL_TEXT_MAX &&
          norm->text[SMS_SPOOL_TEXT_MAX] == '\0');
    CHECK("defensive: peer truncated with NUL",
          norm && strlen(norm->peer) == SMS_SPOOL_PEER_MAX - 1 &&
          norm->peer[SMS_SPOOL_PEER_MAX - 1] == '\0');
    size_t normalized_len = sms_spool_encode(&g_scratch, g_blob2, sizeof(g_blob2));
    sms_spool_init(&g_store);
    bool corrupt = true;
    unsigned got = sms_spool_decode(&g_store, g_blob2, normalized_len, &corrupt);
    CHECK("defensive: clamped entry round trips",
          got == 1 && !corrupt && sms_spool_count(&g_store) == 1 &&
          sms_spool_at(&g_store, 0) &&
          sms_spool_at(&g_store, 0)->text_len == SMS_SPOOL_TEXT_MAX);

    CHECK("defensive: null store count", sms_spool_count(NULL) == 0);
    CHECK("defensive: null store at", sms_spool_at(NULL, 0) == NULL);
    CHECK("defensive: null store find", sms_spool_find(NULL, 1) == NULL);
    CHECK("defensive: null store full", !sms_spool_is_full(NULL));
    CHECK("defensive: null store all durable", !sms_spool_all_durable(NULL));
    CHECK("defensive: null store add", sms_spool_add(NULL, &e) == NULL);
    CHECK("defensive: null entry add", sms_spool_add(&g_scratch, NULL) == NULL);
    CHECK("defensive: null store mark", !sms_spool_mark_durable(NULL, 1, true));
    CHECK("defensive: null store remove", !sms_spool_remove(NULL, 1));
    CHECK("defensive: null store encode", sms_spool_encode(NULL, g_blob2, MAX_BLOB) == 0);
    corrupt = false;
    CHECK("defensive: null store decode",
          sms_spool_decode(NULL, g_blob, normalized_len, &corrupt) == 0 && corrupt);
    corrupt = false;
    CHECK("defensive: null buffer decode",
          sms_spool_decode(&g_scratch, NULL, normalized_len, &corrupt) == 0 && corrupt);
    sms_spool_init(NULL);
    CHECK("defensive: null init is a no-op", true);
}

static void test_persist_decision(void)
{
    /* No pending events: never touch the flash. */
    CHECK("persist: nothing pending", !sms_spool_should_persist(0, 8, 0, 1000, 600000));
    CHECK("persist: nothing pending after a write",
          !sms_spool_should_persist(0, 8, 500, 1000, 600000));
    /* First parked event is written right away (it may only live in RAM otherwise). */
    CHECK("persist: first event", sms_spool_should_persist(1, 8, 0, 1000, 600000));
    /* A handful of events below the trigger is still RAM-only. */
    CHECK("persist: few events", !sms_spool_should_persist(3, 8, 1000, 2000, 600000));
    CHECK("persist: just below trigger", !sms_spool_should_persist(7, 8, 1000, 2000, 600000));
    CHECK("persist: at trigger", sms_spool_should_persist(8, 8, 1000, 2000, 600000));
    CHECK("persist: above trigger", sms_spool_should_persist(9, 8, 1000, 2000, 600000));
    /* Period expiry flushes whatever is pending. */
    CHECK("persist: period not reached", !sms_spool_should_persist(2, 8, 1000, 1000 + 599999, 600000));
    CHECK("persist: period reached", sms_spool_should_persist(2, 8, 1000, 1000 + 600000, 600000));
    CHECK("persist: period passed", sms_spool_should_persist(2, 8, 1000, 900000, 600000));
    /* min_entries == 0 disables the size trigger (period only). */
    CHECK("persist: no size trigger below period",
          !sms_spool_should_persist(63, 0, 1000, 2000, 600000));
    CHECK("persist: no size trigger at period",
          sms_spool_should_persist(63, 0, 1000, 601000, 600000));
    /* A zero period cannot loop-spin: any pending event is written. */
    CHECK("persist: zero period writes pending", sms_spool_should_persist(1, 8, 1000, 1001, 0));
    /* A backwards timestamp must not be read as "a period elapsed". */
    CHECK("persist: backwards clock does not force a write",
          !sms_spool_should_persist(1, 8, 5000, 1000, 600000));
}

int main(void)
{
    test_empty_store();
    test_add_order_dedup();
    test_full_and_remove();
    test_durability();
    test_roundtrip();
    test_corruption();
    test_encode_capacity();
    test_defensive_input();
    test_persist_decision();

    printf("[SUMMARY] %u checks, %d failed\n", s_checks, s_failures);
    if (s_failures) {
        printf("[FAIL] SMS spool core tests\n");
        return 1;
    }
    printf("[PASS] SMS spool core tests\n");
    return 0;
}
