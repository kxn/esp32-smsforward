#include "net_health.h"

#include <stdint.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "feishu.h"
#include "firmware_update.h"
#include "net_health_policy.h"
#include "wifi.h"

static const char *TAG = "net_health";

#define SUPERVISOR_PERIOD_MS 5000
#define SUPERVISOR_STACK 5120
#define SUPERVISOR_PRIO 3
/* Probe often while something is wrong, rarely while everything works: each
 * probe costs a DNS lookup plus a TCP handshake. */
#define PROBE_PERIOD_UNHEALTHY_MS (20 * 1000)
#define PROBE_PERIOD_HEALTHY_MS (60 * 1000)
#define PROBE_TIMEOUT_MS 3000
/* esp_restart() does not wait for the UART: whatever is still buffered is lost.
 * The delay below exists to let the "restarting the device" line above drain,
 * because the owner reads that line to explain the sudden restart. */
#define RESTART_LOG_DRAIN_MS 500

/* Survives the software restart itself so an unattended device cannot end up in a
 * restart loop.  RTC_NOINIT memory is *not* initialised on a power-on reset, so
 * the magic is only trusted together with ESP_RST_SW (see restart_recently_used()). */
RTC_NOINIT_ATTR static uint32_t s_auto_restart_magic;
RTC_NOINIT_ATTR static int64_t s_auto_restart_unix;
#define AUTO_RESTART_MAGIC 0x52425431u

/* Written by the supervisor task, read by the RPC task (status JSON): guard the
 * multi-field state so callers never see half an update. */
static net_health_state_t s_state;
static SemaphoreHandle_t s_state_lock;
static volatile bool s_internet_ok;
static volatile bool s_dns_ok;
static volatile bool s_tcp_ok;
static volatile bool s_auto_restarted;

static void state_lock(void)
{
    if (s_state_lock) xSemaphoreTake(s_state_lock, portMAX_DELAY);
}

static void state_unlock(void)
{
    if (s_state_lock) xSemaphoreGive(s_state_lock);
}

static bool restart_recently_used(void)
{
    /* Only a software reset can have been issued by us: after a power-on the
     * RTC_NOINIT area holds garbage, and a leftover value that happens to match
     * the magic must not disable the restart rung for a whole boot. */
    if (esp_reset_reason() != ESP_RST_SW) return false;
    if (s_auto_restart_magic != AUTO_RESTART_MAGIC) return false;

    int64_t recorded = s_auto_restart_unix;
    int64_t now = (int64_t)time(NULL);
    /* The record can only be dated when the clock was set on both sides of the
     * restart: a restart issued while the clock was still at 1970 -- and a clock
     * that was stepped backwards afterwards -- leave an undatable record.  Treat
     * that as "just happened" so the restart rung stays disabled instead of
     * looping; the cooldown is a safety net, not a precise timer. */
    if (recorded <= 0 || !time_synced() || now < recorded) return true;
    int64_t age_ms = (now - recorded) * 1000;
    return age_ms < NET_HEALTH_REBOOT_COOLDOWN_MS;
}

static void note_auto_restart(void)
{
    s_auto_restart_magic = AUTO_RESTART_MAGIC;
    s_auto_restart_unix = (int64_t)time(NULL);
    /* No s_auto_restarted = true here: esp_restart() follows immediately, so the
     * flag would be a dead store.  The next boot derives it from this record in
     * net_health_start(). */
}

static void apply_action(net_action_t action)
{
    switch (action) {
    case NET_ACTION_WS_REFETCH:
        /* Tear the long connection down; net_task re-fetches a fresh endpoint. */
        feishu_ws_force_reconnect();
        break;
    case NET_ACTION_SNTP_RESTART:
        ESP_LOGW(TAG, "restarting time sync");
        (void)time_sync_restart();
        break;
    case NET_ACTION_DHCP_RENEW:
        ESP_LOGW(TAG, "renewing DHCP lease");
        (void)wifi_dhcp_renew();
        break;
    case NET_ACTION_WIFI_REASSOC:
        ESP_LOGW(TAG, "re-associating Wi-Fi");
        (void)wifi_reassociate();
        break;
    case NET_ACTION_REBOOT:
        if (firmware_busy()) {
            /* Race guard only: the tick above already blocks the restart rung while
             * a firmware operation is in flight, so this is just the window between
             * that observation and the action.  The device will restart once the
             * operation finishes or the next outage tick retries. */
            ESP_LOGW(TAG, "restart wanted but a firmware operation is in flight");
            break;
        }
        ESP_LOGE(TAG, "upstream unreachable for too long, restarting the device");
        note_auto_restart();
        vTaskDelay(pdMS_TO_TICKS(RESTART_LOG_DRAIN_MS));
        esp_restart();
        break;
    default:
        break;
    }
}

static void supervisor_task(void *arg)
{
    (void)arg;
    int64_t next_probe_ms = 0;
    while (1) {
        int64_t now = esp_timer_get_time() / 1000;
        bool link = wifi_connected();

        if (!link) {
            s_internet_ok = false;
            s_dns_ok = false;
            s_tcp_ok = false;
            next_probe_ms = 0;
        } else if (now >= next_probe_ms) {
            bool dns_ok = false, tcp_ok = false;
            /* The probe host/port (open.feishu.cn:443) lives in wifi.c: the
             * reachability definition belongs next to the transport it exercises. */
            bool ok = wifi_probe_internet(&dns_ok, &tcp_ok, PROBE_TIMEOUT_MS);
            s_dns_ok = dns_ok;
            s_tcp_ok = tcp_ok;
            s_internet_ok = ok;
            next_probe_ms = now + (ok ? PROBE_PERIOD_HEALTHY_MS : PROBE_PERIOD_UNHEALTHY_MS);
            if (!ok) {
                ESP_LOGW(TAG, "probe failed: dns=%d tcp=%d", dns_ok, tcp_ok);
            }
        }

        net_health_input_t in = {
            /* A device whose push channel is not configured yet has nothing to
             * repair; escalating there would restart the box during provisioning. */
            .supervise = feishu_configured(),
            .wifi_connected = link,
            .internet_ok = s_internet_ok,
            .ws_connected = feishu_ws_connected(),
            /* feishu_ws_alive() implies connected, so the stale flag is derived
             * from it alone; net_health_tick() still requires ws_connected before
             * treating a stale socket as half-open (a detached socket belongs to
             * net_task). */
            .ws_rx_stale = !feishu_ws_alive(),
            .time_synced = time_synced(),
            /* Two independent reasons to keep the restart rung unspent: the 2 h
             * cooldown after our own restart, and a firmware operation that is in
             * flight (it restarts the device itself, or finishes shortly). */
            .reboot_blocked = restart_recently_used() || firmware_busy(),
        };

        state_lock();
        net_action_t action = net_health_tick(&s_state, &in, now);
        int64_t down_ms = s_state.down_since_ms ? now - s_state.down_since_ms : 0;
        uint32_t rung = s_state.rung;
        uint32_t outages = s_state.outages;
        state_unlock();

        if (action != NET_ACTION_NONE) {
            ESP_LOGW(TAG, "escalating: action=%d rung=%u down_ms=%lld outages=%u",
                     (int)action, (unsigned)rung, (long long)down_ms, (unsigned)outages);
            /* Repairs can block (flash, DHCP, a restart): never run them with the
             * state lock held. */
            apply_action(action);
        }
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));
    }
}

void net_health_start(void)
{
    if (!s_state_lock) {
        s_state_lock = xSemaphoreCreateMutex();
        if (!s_state_lock) {
            ESP_LOGE(TAG, "cannot create state lock; status reads may tear");
        }
    }
    net_health_reset(&s_state);
    /* The boot itself is not an outage.  Arm the clock right away so a device that
     * comes up without a network still climbs the ladder, but a normal boot does
     * not report an internet outage that never happened (`outages` would otherwise
     * be at least 1 on every healthy device). */
    s_state.down_since_ms = esp_timer_get_time() / 1000;
    if (restart_recently_used()) {
        s_auto_restarted = true;
        ESP_LOGW(TAG, "automatic restart happened recently; escalation to restart "
                      "is disabled until the cooldown expires");
    }
    if (!xTaskCreate(supervisor_task, "net_health", SUPERVISOR_STACK, NULL,
                     SUPERVISOR_PRIO, NULL)) {
        ESP_LOGE(TAG, "cannot start health supervisor");
    }
}

bool net_health_internet_ok(void)
{
    /* Single byte, written by the supervisor: atomic enough for the replay gate in
     * sms_spool.c, and not worth taking the state lock for. */
    return s_internet_ok;
}

void net_health_status_json(cJSON *parent)
{
    if (!parent) return;

    /* Take a coherent snapshot under the lock the supervisor writes with, then
     * release it before any cJSON work: allocations must not run with the lock
     * held.  Before net_health_start() there is no writer, so a missing lock is
     * harmless. */
    net_health_state_t snapshot;
    bool internet_ok;
    bool dns_ok;
    bool tcp_ok;
    state_lock();
    snapshot = s_state;
    internet_ok = s_internet_ok;
    dns_ok = s_dns_ok;
    tcp_ok = s_tcp_ok;
    state_unlock();

    int64_t now = esp_timer_get_time() / 1000;
    cJSON_AddBoolToObject(parent, "internet_ok", internet_ok);
    cJSON_AddBoolToObject(parent, "dns_ok", dns_ok);
    cJSON_AddBoolToObject(parent, "tcp_ok", tcp_ok);
    cJSON_AddNumberToObject(parent, "down_ms",
                            snapshot.down_since_ms ? (double)(now - snapshot.down_since_ms) : 0);
    cJSON_AddNumberToObject(parent, "outages", (double)snapshot.outages);
    cJSON_AddNumberToObject(parent, "last_action", (double)snapshot.last_action);
    cJSON_AddBoolToObject(parent, "auto_restarted", s_auto_restarted);
}
