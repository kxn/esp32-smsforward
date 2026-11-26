#include "peer_mark.h"

#include <string.h>

#include "cfg.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "peer_mark_core.h"

static const char *TAG = "peer_mark";

/* Responses are small JSON documents; anything larger is not a mark reply. */
#define RESPONSE_MAX 2048
#define CACHE_ENTRIES 32
/* A real mark rarely disappears within a day; "no mark" is re-asked sooner so a
 * number that gets reported today shows up tomorrow. */
#define CACHE_TTL_MS (24 * 60 * 60 * 1000LL)
#define CACHE_MISS_TTL_MS (6 * 60 * 60 * 1000LL)
#define TIMEOUT_MIN_MS 500
#define TIMEOUT_MAX_MS 5000

typedef struct {
    char number[PEER_MARK_NUMBER_MAX];
    char label[PEER_MARK_LABEL_MAX];
    int64_t at_ms;
    bool found;
    bool used;
} cache_entry_t;

static cache_entry_t s_cache[CACHE_ENTRIES];
static unsigned s_cache_next;
static char s_result[PEER_MARK_LABEL_MAX];
static unsigned s_queries, s_timeouts, s_failures;
static uint32_t s_last_ms;
static unsigned s_last_bytes;
static uint32_t s_cfg_hash;
static SemaphoreHandle_t s_lock;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t hash_cfg(const app_cfg_t *cfg)
{
    uint32_t h = 2166136261u;
    const char *parts[2] = { cfg->mark_url, cfg->mark_key };
    for (int i = 0; i < 2; i++) {
        for (const char *p = parts[i]; p && *p; p++) {
            h ^= (uint8_t)*p;
            h *= 16777619u;
        }
        h ^= 0xFF;
        h *= 16777619u;
    }
    return h;
}

/* Providers expect a bare dial string; the core keeps the canonical form so it
 * is host-testable. */
static bool normalise_number(const char *in, char *out, size_t cap)
{
    return peer_mark_normalise_number(in, out, cap) > 0;
}

static void cache_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void cache_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

void peer_mark_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

bool peer_mark_enabled(void)
{
    const app_cfg_t *cfg = cfg_get();
    return cfg && cfg->mark_url[0] != '\0';
}

/* Returns true with the cached answer in *found (and s_result when found). */
static bool cache_lookup(const char *number, bool *found)
{
    int64_t now = now_ms();
    bool hit = false;
    cache_lock();
    for (unsigned i = 0; i < CACHE_ENTRIES; i++) {
        cache_entry_t *e = &s_cache[i];
        if (!e->used || strcmp(e->number, number) != 0) continue;
        int64_t ttl = e->found ? CACHE_TTL_MS : CACHE_MISS_TTL_MS;
        if (now - e->at_ms > ttl) {
            e->used = false;
            break;
        }
        if (e->found) strlcpy(s_result, e->label, sizeof(s_result));
        *found = e->found;
        hit = true;
        break;
    }
    cache_unlock();
    return hit;
}

static void cache_store(const char *number, const char *label, bool found)
{
    cache_lock();
    cache_entry_t *e = &s_cache[s_cache_next];
    s_cache_next = (s_cache_next + 1) % CACHE_ENTRIES;
    strlcpy(e->number, number, sizeof(e->number));
    strlcpy(e->label, found ? label : "", sizeof(e->label));
    e->at_ms = now_ms();
    e->found = found;
    e->used = true;
    cache_unlock();
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} body_ctx_t;

/* Copies at most cap-1 body bytes; a longer reply is reported as a failure rather
 * than parsed from a truncated document. */
static esp_err_t body_event(esp_http_client_event_t *ev)
{
    body_ctx_t *ctx = ev->user_data;
    if (ev->event_id != HTTP_EVENT_ON_DATA || !ctx || !ev->data || ev->data_len <= 0) return ESP_OK;
    size_t room = ctx->cap - 1 - ctx->len;
    size_t take = (size_t)ev->data_len < room ? (size_t)ev->data_len : room;
    memcpy(ctx->buf + ctx->len, ev->data, take);
    ctx->len += take;
    if ((size_t)ev->data_len > take) ctx->overflow = true;
    return ESP_OK;
}

static void note_result(const char *number, const char *label, bool found, uint32_t elapsed)
{
    s_last_ms = elapsed;
    cache_store(number, label, found);
    if (found) strlcpy(s_result, label, sizeof(s_result));
    else s_result[0] = '\0';
}

const char *peer_mark_lookup(const char *number)
{
    const app_cfg_t *cfg = cfg_get();
    if (!cfg || !cfg->mark_url[0] || !number || !number[0]) return NULL;

    char normal[PEER_MARK_NUMBER_MAX];
    if (!normalise_number(number, normal, sizeof(normal))) return NULL;

    uint32_t h = hash_cfg(cfg);
    cache_lock();
    if (h != s_cfg_hash) {
        /* The owner changed the service; stale answers are not comparable. */
        memset(s_cache, 0, sizeof(s_cache));
        s_cache_next = 0;
        s_cfg_hash = h;
    }
    cache_unlock();

    bool found = false;
    if (cache_lookup(normal, &found)) return found ? s_result : NULL;

    char url[PEER_MARK_URL_MAX];
    if (!peer_mark_build_url(url, sizeof(url), cfg->mark_url, cfg->mark_key, normal)) {
        ESP_LOGW(TAG, "mark URL template rejected (check {number} placement)");
        s_failures++;
        return NULL;
    }

    unsigned budget = cfg->mark_timeout_ms;
    if (budget < TIMEOUT_MIN_MS) budget = TIMEOUT_MIN_MS;
    if (budget > TIMEOUT_MAX_MS) budget = TIMEOUT_MAX_MS;

    s_queries++;
    int64_t start = now_ms();
    char body[RESPONSE_MAX];
    body_ctx_t ctx = { .buf = body, .cap = sizeof(body), .len = 0, .overflow = false };

    esp_http_client_config_t sc = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)budget,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .event_handler = body_event,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&sc);
    if (!client) {
        ESP_LOGW(TAG, "mark client init failed");
        s_failures++;
        return NULL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    uint32_t elapsed = (uint32_t)(now_ms() - start);
    body[ctx.len] = '\0';
    /* Diagnostics describe the latest attempt, even when it produced no label. */
    s_last_ms = elapsed;
    s_last_bytes = (unsigned)ctx.len;
    if (err != ESP_OK) {
        if (elapsed + 20 >= budget) {
            s_timeouts++;
            ESP_LOGW(TAG, "mark lookup timed out after %ums (budget %ums)", (unsigned)elapsed,
                     budget);
        } else {
            s_failures++;
            ESP_LOGW(TAG, "mark request failed after %ums: %s", (unsigned)elapsed,
                     esp_err_to_name(err));
        }
        return NULL;
    }
    if (status != 200) {
        s_failures++;
        ESP_LOGW(TAG, "mark http=%d after %ums", status, (unsigned)elapsed);
        return NULL;
    }
    if (ctx.overflow) {
        s_failures++;
        ESP_LOGW(TAG, "mark reply larger than %d bytes", RESPONSE_MAX - 1);
        return NULL;
    }

    char label[PEER_MARK_LABEL_MAX];
    label[0] = '\0';
    s_last_bytes = (unsigned)ctx.len;
    if (ctx.len) ESP_LOGD(TAG, "mark body: %.*s", (int)(ctx.len > 64 ? 64 : ctx.len), body);
    bool marked = peer_mark_parse_labels(body, ctx.len, label, sizeof(label));
    note_result(normal, label, marked, elapsed);
    if (!marked) {
        ESP_LOGD(TAG, "mark: no label in %ums (%u bytes)", (unsigned)elapsed, (unsigned)ctx.len);
        return NULL;
    }
    ESP_LOGI(TAG, "mark: %s -> %s (%ums)", normal, label, (unsigned)elapsed);
    return s_result;
}

unsigned peer_mark_queries(void) { return s_queries; }
unsigned peer_mark_timeouts(void) { return s_timeouts; }
unsigned peer_mark_failures(void) { return s_failures; }
uint32_t peer_mark_last_ms(void) { return s_last_ms; }
unsigned peer_mark_last_bytes(void) { return s_last_bytes; }
const char *peer_mark_last_label(void) { return s_result; }

unsigned peer_mark_cache_entries(void)
{
    unsigned n = 0;
    cache_lock();
    for (unsigned i = 0; i < CACHE_ENTRIES; i++) {
        if (s_cache[i].used) n++;
    }
    cache_unlock();
    return n;
}
