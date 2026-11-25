#include "sms_spool_core.h"

#include <stddef.h>
#include <string.h>

/* Persisted blob layout, all integers little endian:
 *
 *   header, 16 bytes
 *      0  magic "SPS1"
 *      4  version (currently 1)
 *      5  reserved (0)
 *      6  entry count, u16
 *      8  payload length in bytes, u32
 *     12  FNV-1a checksum, u32, over bytes [0,12) followed by the payload
 *   payload: `count` variable length entry records, oldest first
 *      0  kind, 1 slot, 2 ts_valid, 3 durable
 *      4  received_unix, i64
 *     12  fingerprint, u64
 *     20  text_len, u16
 *     22  peer_len, record_id_len, msg_id_len, reserved (one byte each)
 *     26  peer bytes, record_id bytes, msg_id bytes, text bytes
 *
 * Strings are stored without their terminating NUL, which the decoder adds
 * back.  The decoder validates the complete frame (magic, version, checksum,
 * count and every entry length) before it touches the store, so a truncated or
 * corrupted blob never leaves a partial entry behind. */

#define SPOOL_VERSION 1u
#define SPOOL_HEADER_SIZE 16u
#define SPOOL_ENTRY_FIXED 26u
#define SPOOL_HEADER_VERSION_OFFSET 4u
#define SPOOL_HEADER_COUNT_OFFSET 6u
#define SPOOL_HEADER_PAYLOAD_LEN_OFFSET 8u
#define SPOOL_CHECKSUM_OFFSET 12u
#define SPOOL_ENTRY_TEXT_LEN_OFFSET 20u
#define SPOOL_ENTRY_PEER_LEN_OFFSET 22u
#define SPOOL_ENTRY_RECORD_ID_LEN_OFFSET 23u
#define SPOOL_ENTRY_MSG_ID_LEN_OFFSET 24u

static const uint8_t s_magic[4] = { 'S', 'P', 'S', '1' };

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint32_t fnv1a(uint32_t hash, const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        hash ^= p[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

/* Checksum covers everything but the checksum field itself, so the header and
 * the payload are protected together. */
static uint32_t blob_checksum(const uint8_t *buf, size_t len)
{
    uint32_t hash = UINT32_C(2166136261);
    hash = fnv1a(hash, buf, SPOOL_CHECKSUM_OFFSET);
    return fnv1a(hash, buf + SPOOL_HEADER_SIZE, len - SPOOL_HEADER_SIZE);
}

/* Length of a C string, never reading more than `max` bytes.  Used so a caller
 * supplied field without a terminator cannot make us walk out of bounds. */
static size_t bounded_strlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return n;
}

/* Copy a fixed size field, truncating to leave room for the terminator. */
static void copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = bounded_strlen(src, cap - 1);
    memmove(dst, src, n);
    dst[n] = '\0';
}

/* Copy the event text, honouring both the declared length and the capacity. */
static void copy_text(sms_spool_entry_t *dst, const char *src, uint16_t src_len)
{
    size_t n = bounded_strlen(src, SMS_SPOOL_TEXT_MAX);
    if ((size_t)src_len < n) n = (size_t)src_len;
    memmove(dst->text, src, n);
    dst->text[n] = '\0';
    dst->text_len = (uint16_t)n;
}

/* Text length as emitted on the wire: never past the first NUL, never past the
 * declared length, never past the field capacity. */
static size_t entry_text_len(const sms_spool_entry_t *e)
{
    size_t n = bounded_strlen(e->text, SMS_SPOOL_TEXT_MAX);
    if ((size_t)e->text_len < n) n = (size_t)e->text_len;
    return n;
}

/* Entry count that is always safe to index with. */
static unsigned store_count(const sms_spool_store_t *s)
{
    if (!s) return 0;
    return s->count > (unsigned)SMS_SPOOL_CAPACITY ? (unsigned)SMS_SPOOL_CAPACITY : s->count;
}

static sms_spool_entry_t *find_entry(sms_spool_store_t *s, uint64_t fingerprint)
{
    if (!s) return NULL;
    unsigned count = store_count(s);
    for (unsigned i = 0; i < count; ++i) {
        if (s->entry[i].fingerprint == fingerprint) return &s->entry[i];
    }
    return NULL;
}

static size_t entry_frame_size(const uint8_t *p)
{
    return (size_t)SPOOL_ENTRY_FIXED + (size_t)get_u16(p + SPOOL_ENTRY_TEXT_LEN_OFFSET) +
           p[SPOOL_ENTRY_PEER_LEN_OFFSET] + p[SPOOL_ENTRY_RECORD_ID_LEN_OFFSET] +
           p[SPOOL_ENTRY_MSG_ID_LEN_OFFSET];
}

/* Walk the payload without writing anywhere: every entry must be complete and
 * within the declared limits, and the walk must consume the payload exactly. */
static bool payload_valid(const uint8_t *payload, size_t payload_len, unsigned declared)
{
    if (declared > (unsigned)SMS_SPOOL_CAPACITY) return false;
    size_t off = 0;
    unsigned seen = 0;
    while (off < payload_len) {
        if (seen >= (unsigned)SMS_SPOOL_CAPACITY) return false;
        if (payload_len - off < (size_t)SPOOL_ENTRY_FIXED) return false;
        const uint8_t *p = payload + off;
        size_t text_len = get_u16(p + SPOOL_ENTRY_TEXT_LEN_OFFSET);
        size_t peer_len = p[SPOOL_ENTRY_PEER_LEN_OFFSET];
        size_t record_id_len = p[SPOOL_ENTRY_RECORD_ID_LEN_OFFSET];
        size_t msg_id_len = p[SPOOL_ENTRY_MSG_ID_LEN_OFFSET];
        if (text_len > (size_t)SMS_SPOOL_TEXT_MAX) return false;
        if (peer_len > (size_t)SMS_SPOOL_PEER_MAX - 1) return false;
        if (record_id_len > (size_t)SMS_SPOOL_ID_MAX - 1) return false;
        if (msg_id_len > (size_t)SMS_SPOOL_ID_MAX - 1) return false;
        size_t frame = entry_frame_size(p);
        if (payload_len - off < frame) return false;
        off += frame;
        ++seen;
    }
    return off == payload_len && seen == declared;
}

/* Fill one previously validated entry; returns the bytes consumed. */
static size_t load_entry(const uint8_t *p, sms_spool_entry_t *dst)
{
    size_t text_len = get_u16(p + SPOOL_ENTRY_TEXT_LEN_OFFSET);
    size_t peer_len = p[SPOOL_ENTRY_PEER_LEN_OFFSET];
    size_t record_id_len = p[SPOOL_ENTRY_RECORD_ID_LEN_OFFSET];
    size_t msg_id_len = p[SPOOL_ENTRY_MSG_ID_LEN_OFFSET];
    size_t off = SPOOL_ENTRY_FIXED;
    memset(dst, 0, sizeof(*dst));
    dst->kind = p[0];
    dst->slot = p[1];
    dst->ts_valid = p[2] ? 1 : 0;
    dst->durable = p[3] ? 1 : 0;
    dst->received_unix = (int64_t)get_u64(p + 4);
    dst->fingerprint = get_u64(p + 12);
    memcpy(dst->peer, p + off, peer_len);
    dst->peer[peer_len] = '\0';
    off += peer_len;
    memcpy(dst->record_id, p + off, record_id_len);
    dst->record_id[record_id_len] = '\0';
    off += record_id_len;
    memcpy(dst->msg_id, p + off, msg_id_len);
    dst->msg_id[msg_id_len] = '\0';
    off += msg_id_len;
    memcpy(dst->text, p + off, text_len);
    dst->text[text_len] = '\0';
    dst->text_len = (uint16_t)text_len;
    return off + text_len;
}

void sms_spool_init(sms_spool_store_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

unsigned sms_spool_count(const sms_spool_store_t *s)
{
    return store_count(s);
}

bool sms_spool_is_full(const sms_spool_store_t *s)
{
    return s && store_count(s) >= (unsigned)SMS_SPOOL_CAPACITY;
}

const sms_spool_entry_t *sms_spool_at(const sms_spool_store_t *s, unsigned index)
{
    if (!s || index >= store_count(s)) return NULL;
    return &s->entry[index];
}

const sms_spool_entry_t *sms_spool_find(const sms_spool_store_t *s, uint64_t fingerprint)
{
    if (!s) return NULL;
    unsigned count = store_count(s);
    for (unsigned i = 0; i < count; ++i) {
        if (s->entry[i].fingerprint == fingerprint) return &s->entry[i];
    }
    return NULL;
}

sms_spool_entry_t *sms_spool_add(sms_spool_store_t *s, const sms_spool_entry_t *e)
{
    if (!s || !e) return NULL;
    sms_spool_entry_t *existing = find_entry(s, e->fingerprint);
    if (existing) {
        /* Same event seen again: keep its position, refresh what may have
         * improved (text, original timestamp, durability). */
        existing->received_unix = e->received_unix;
        existing->durable = e->durable ? 1 : 0;
        copy_text(existing, e->text, e->text_len);
        return existing;
    }
    if (store_count(s) >= (unsigned)SMS_SPOOL_CAPACITY) {
        ++s->dropped;
        return NULL;
    }
    sms_spool_entry_t *dst = &s->entry[s->count];
    memset(dst, 0, sizeof(*dst));
    dst->kind = e->kind;
    dst->slot = e->slot;
    dst->ts_valid = e->ts_valid ? 1 : 0;
    dst->durable = e->durable ? 1 : 0;
    dst->received_unix = e->received_unix;
    dst->fingerprint = e->fingerprint;
    copy_field(dst->peer, sizeof(dst->peer), e->peer);
    copy_field(dst->record_id, sizeof(dst->record_id), e->record_id);
    copy_field(dst->msg_id, sizeof(dst->msg_id), e->msg_id);
    copy_text(dst, e->text, e->text_len);
    ++s->count;
    ++s->added;
    return dst;
}

bool sms_spool_mark_durable(sms_spool_store_t *s, uint64_t fingerprint, bool durable)
{
    sms_spool_entry_t *e = find_entry(s, fingerprint);
    if (!e) return false;
    e->durable = durable ? 1 : 0;
    return true;
}

bool sms_spool_all_durable(const sms_spool_store_t *s)
{
    if (!s) return false;
    unsigned count = store_count(s);
    for (unsigned i = 0; i < count; ++i) {
        if (!s->entry[i].durable) return false;
    }
    return true;
}

bool sms_spool_should_persist(unsigned pending, unsigned min_entries,
                              int64_t last_persist_ms, int64_t now_ms,
                              int64_t period_ms)
{
    if (pending == 0) return false;
    if (last_persist_ms <= 0) return true;             /* first pending event */
    if (min_entries > 0 && pending >= min_entries) return true;
    if (period_ms <= 0) return true;
    return now_ms - last_persist_ms >= period_ms;
}

bool sms_spool_remove(sms_spool_store_t *s, uint64_t fingerprint)
{
    if (!s) return false;
    unsigned count = store_count(s);
    for (unsigned i = 0; i < count; ++i) {
        if (s->entry[i].fingerprint != fingerprint) continue;
        if (i + 1 < count) {
            memmove(&s->entry[i], &s->entry[i + 1],
                    (size_t)(count - i - 1) * sizeof(s->entry[0]));
        }
        memset(&s->entry[count - 1], 0, sizeof(s->entry[0]));
        s->count = count - 1;
        return true;
    }
    return false;
}

size_t sms_spool_encoded_max(unsigned count)
{
    if (count > (unsigned)SMS_SPOOL_CAPACITY) count = (unsigned)SMS_SPOOL_CAPACITY;
    size_t frame = (size_t)SPOOL_ENTRY_FIXED + (size_t)(SMS_SPOOL_PEER_MAX - 1) +
                   (size_t)(SMS_SPOOL_ID_MAX - 1) * 2 + (size_t)SMS_SPOOL_TEXT_MAX;
    return (size_t)SPOOL_HEADER_SIZE + (size_t)count * frame;
}

size_t sms_spool_encode(const sms_spool_store_t *s, uint8_t *out, size_t cap)
{
    if (!s) return 0;
    unsigned count = store_count(s);
    size_t payload = 0;
    for (unsigned i = 0; i < count; ++i) {
        const sms_spool_entry_t *e = &s->entry[i];
        size_t frame = (size_t)SPOOL_ENTRY_FIXED + bounded_strlen(e->peer, SMS_SPOOL_PEER_MAX - 1) +
                       bounded_strlen(e->record_id, SMS_SPOOL_ID_MAX - 1) +
                       bounded_strlen(e->msg_id, SMS_SPOOL_ID_MAX - 1) + entry_text_len(e);
        if (payload > SIZE_MAX - frame) return 0;
        payload += frame;
    }
    if (payload > UINT32_MAX) return 0;
    if (!out || cap < (size_t)SPOOL_HEADER_SIZE || cap - SPOOL_HEADER_SIZE < payload) return 0;

    memcpy(out, s_magic, sizeof(s_magic));
    out[SPOOL_HEADER_VERSION_OFFSET] = (uint8_t)SPOOL_VERSION;
    out[SPOOL_HEADER_VERSION_OFFSET + 1] = 0;
    put_u16(out + SPOOL_HEADER_COUNT_OFFSET, (uint16_t)count);
    put_u32(out + SPOOL_HEADER_PAYLOAD_LEN_OFFSET, (uint32_t)payload);
    uint8_t *p = out + SPOOL_HEADER_SIZE;
    for (unsigned i = 0; i < count; ++i) {
        const sms_spool_entry_t *e = &s->entry[i];
        size_t peer_len = bounded_strlen(e->peer, SMS_SPOOL_PEER_MAX - 1);
        size_t record_id_len = bounded_strlen(e->record_id, SMS_SPOOL_ID_MAX - 1);
        size_t msg_id_len = bounded_strlen(e->msg_id, SMS_SPOOL_ID_MAX - 1);
        size_t text_len = entry_text_len(e);
        p[0] = e->kind;
        p[1] = e->slot;
        p[2] = e->ts_valid ? 1 : 0;
        p[3] = e->durable ? 1 : 0;
        put_u64(p + 4, (uint64_t)e->received_unix);
        put_u64(p + 12, e->fingerprint);
        put_u16(p + SPOOL_ENTRY_TEXT_LEN_OFFSET, (uint16_t)text_len);
        p[SPOOL_ENTRY_PEER_LEN_OFFSET] = (uint8_t)peer_len;
        p[SPOOL_ENTRY_RECORD_ID_LEN_OFFSET] = (uint8_t)record_id_len;
        p[SPOOL_ENTRY_MSG_ID_LEN_OFFSET] = (uint8_t)msg_id_len;
        p[SPOOL_ENTRY_MSG_ID_LEN_OFFSET + 1] = 0;
        memcpy(p + SPOOL_ENTRY_FIXED, e->peer, peer_len);
        memcpy(p + SPOOL_ENTRY_FIXED + peer_len, e->record_id, record_id_len);
        memcpy(p + SPOOL_ENTRY_FIXED + peer_len + record_id_len, e->msg_id, msg_id_len);
        memcpy(p + SPOOL_ENTRY_FIXED + peer_len + record_id_len + msg_id_len, e->text, text_len);
        p += SPOOL_ENTRY_FIXED + peer_len + record_id_len + msg_id_len + text_len;
    }
    put_u32(out + SPOOL_CHECKSUM_OFFSET, blob_checksum(out, SPOOL_HEADER_SIZE + payload));
    return SPOOL_HEADER_SIZE + payload;
}

unsigned sms_spool_decode(sms_spool_store_t *s, const uint8_t *buf, size_t len, bool *corrupt)
{
    if (corrupt) *corrupt = true;
    if (!s || !buf || len < (size_t)SPOOL_HEADER_SIZE) return 0;
    if (memcmp(buf, s_magic, sizeof(s_magic)) != 0) return 0;
    if (buf[SPOOL_HEADER_VERSION_OFFSET] != (uint8_t)SPOOL_VERSION) return 0;
    unsigned declared = get_u16(buf + SPOOL_HEADER_COUNT_OFFSET);
    uint32_t payload_len = get_u32(buf + SPOOL_HEADER_PAYLOAD_LEN_OFFSET);
    if (declared > (unsigned)SMS_SPOOL_CAPACITY) return 0;
    if ((size_t)payload_len != len - SPOOL_HEADER_SIZE) return 0;
    if (get_u32(buf + SPOOL_CHECKSUM_OFFSET) != blob_checksum(buf, len)) return 0;
    const uint8_t *payload = buf + SPOOL_HEADER_SIZE;
    if (!payload_valid(payload, payload_len, declared)) return 0;
    if (corrupt) *corrupt = false;

    /* Everything below only reads validated bytes, so the store can be filled
     * incrementally.  Entries already present (same fingerprint) are skipped so
     * a replay of a half-delivered journal cannot create duplicates. */
    unsigned restored = 0;
    size_t off = 0;
    for (unsigned i = 0; i < declared; ++i) {
        const uint8_t *p = payload + off;
        size_t frame = entry_frame_size(p);
        if (sms_spool_find(s, get_u64(p + 12)) != NULL) {
            off += frame;
            continue;
        }
        if (s->count >= (unsigned)SMS_SPOOL_CAPACITY) {
            s->dropped += declared - i;
            break;
        }
        load_entry(p, &s->entry[s->count]);
        ++s->count;
        ++s->added;
        ++restored;
        off += frame;
    }
    return restored;
}
