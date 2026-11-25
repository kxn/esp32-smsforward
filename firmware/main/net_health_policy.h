#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Escalation ladder for "Wi-Fi is up but the internet (and therefore Feishu) is
 * not reachable".
 *
 * Field failure it addresses: the device kept its LAN address and its web console
 * kept answering, while every upstream connection (Feishu long connection and
 * Feishu REST) stayed dead for ~30 hours.  Nothing in the firmware ever probed
 * the internet, re-associated Wi-Fi, renewed the DHCP lease or restarted SNTP, so
 * the only recovery was the owner power cycling the device.
 *
 * This header is a pure state machine: the supervisor in net_health.c feeds it the
 * observed state every few seconds and executes the single action it returns.
 * Everything here is host-testable (see tools/test_net_health_policy.c).
 *
 * Division of labour, so the two loops never fight:
 *   - net_task owns the long connection: it rebuilds it whenever
 *     feishu_ws_connected() is false and retries every few seconds;
 *   - this ladder owns the internet path and the clock.  It escalates only while
 *     the internet is unreachable or the clock has never been set, plus the one
 *     connection failure net_task cannot see (a socket that still claims to be
 *     connected but delivers nothing -> NET_ACTION_WS_REFETCH).
 *
 * What arms the escalation clock (s->down_since_ms): internet or clock trouble
 * only, i.e. !wifi_connected || !internet_ok || !time_synced.  A long connection
 * that is merely down or silent on a working internet must NOT age the ladder.
 * It used to: the reset condition was net_health_healthy(), which is false
 * whenever the WS is down, so a 30 h WS-only outage left down_ms at 30 h and the
 * first failed probe after it jumped straight to NET_ACTION_REBOOT, skipping
 * SNTP/DHCP/re-association and burning the restart budget.  The clock is now
 * closed as soon as link + probe + clock are fine again (net_health_ladder_calm()),
 * and s->outages counts those internet-outage episodes only, so WS blips no
 * longer inflate status.net.outages.
 *
 * Ladder for one outage, cheapest action first, one rung per action (a long
 * period without a decision, e.g. while the link was down, must not skip the
 * cheap repairs):
 *   120 s  -> restart SNTP
 *   300 s  -> renew the DHCP lease (a lost NAT/upstream mapping survives it)
 *   600 s  -> drop and re-associate the Wi-Fi link
 *  1200 s  -> renew DHCP again after the re-association
 *  2700 s  -> controlled restart, i.e. what the owner used to do by hand
 * The restart happens at most once per NET_HEALTH_REBOOT_COOLDOWN_MS so that a
 * genuine multi-hour provider outage cannot turn into a reboot loop.
 *
 * What rung 1 actually covers: "no internet at all" and "the clock was never set
 * since boot" (time_synced() is a latch that only an earlier SNTP restart clears).
 * It does NOT cover a clock that is set but wrong: nothing in the supervisor
 * reads the wall clock or validates TLS (the probe is DNS + plain TCP:443), so a
 * drifted clock is invisible here.  Drift surfaces only in the Feishu TLS layer
 * as certificate-validity failures, which the supervisor can neither observe nor
 * repair.  A clock that never syncs is also indistinguishable from a wedged SNTP
 * client, so the rungs above 1 will not help it; the restart cooldown is what
 * bounds how often such a device can restart itself. */

#define NET_HEALTH_SNTP_MS (120 * 1000)
#define NET_HEALTH_DHCP_MS (5 * 60 * 1000)
#define NET_HEALTH_REASSOC_MS (10 * 60 * 1000)
#define NET_HEALTH_DHCP_AGAIN_MS (20 * 60 * 1000)
#define NET_HEALTH_REBOOT_MS (45 * 60 * 1000)
#define NET_HEALTH_REBOOT_COOLDOWN_MS (2 * 60 * 60 * 1000)

/* The clock never synced but the internet answers: only SNTP can help.  The path
 * itself is proven good, so the ladder never escalates past rung 1 (an NTP-blocked
 * network would otherwise restart the device every hour for no reason), and the
 * cheap repair is simply retried at this cadence until it works. */
#define NET_HEALTH_CLOCK_RETRY_MS (10 * 60 * 1000)

/* How long to wait between two repairs of the same kind. */
#define NET_HEALTH_ACTION_COOLDOWN_MS (45 * 1000)

typedef enum {
    NET_ACTION_NONE = 0,
    NET_ACTION_WS_REFETCH,     /* mark the long connection dead; net_task rebuilds it */
    NET_ACTION_SNTP_RESTART,   /* restart the SNTP client */
    NET_ACTION_DHCP_RENEW,     /* force a fresh DHCP lease */
    NET_ACTION_WIFI_REASSOC,   /* esp_wifi_disconnect() + esp_wifi_connect() */
    NET_ACTION_REBOOT,         /* controlled esp_restart() */
} net_action_t;

/* Ladder rungs in the order they are used; one per threshold above.  Kept explicit
 * so the comparison in net_health_tick() does not repeat magic numbers. */
typedef enum {
    NET_RUNG_NONE = 0,
    NET_RUNG_SNTP = 1,
    NET_RUNG_DHCP = 2,
    NET_RUNG_REASSOC = 3,
    NET_RUNG_DHCP_AGAIN = 4,
    NET_RUNG_REBOOT = 5,
} net_rung_t;

typedef struct {
    /* False when there is nothing to supervise (Feishu is not configured yet, or
     * the device is still being provisioned over BLE).  An unsupervised device
     * must never walk the ladder: it would restart itself while the owner is
     * still typing the app secret in. */
    bool supervise;
    bool wifi_connected;   /* link + address still present */
    bool internet_ok;      /* DNS + TCP:443 reachable */
    bool ws_connected;     /* Feishu long connection reports connected */
    bool ws_rx_stale;      /* feishu_ws_alive() == false; only meaningful together
                            * with ws_connected, i.e. a connected-but-silent socket */
    bool time_synced;      /* clock has been set at least once since boot */
    bool reboot_blocked;   /* the restart rung must not be used now: an automatic
                            * restart happened recently, or a firmware operation is
                            * in flight.  Never spent: the rung is retried later. */
} net_health_input_t;

typedef struct {
    int64_t down_since_ms;   /* 0 while healthy, else when the outage started */
    uint32_t rung;           /* ladder steps used for the current outage, net_rung_t */
    int64_t last_action_ms;  /* when the last repair was requested */
    net_action_t last_action;
    uint32_t outages;        /* statistics: internet-outage episodes, not WS blips */
} net_health_state_t;

static inline void net_health_reset(net_health_state_t *s)
{
    if (!s) return;
    s->down_since_ms = 0;
    s->rung = 0;
    s->last_action_ms = -NET_HEALTH_ACTION_COOLDOWN_MS;
    s->last_action = NET_ACTION_NONE;
    s->outages = 0;
}

/* Everything, including the long connection, looks fine.  Callers use this to tell
 * "quiet" from "quiet because the connection layer is being rebuilt"; it is
 * deliberately *not* the escalation reset condition, because a dead WS on a
 * working internet must not arm the ladder (see the comment block above). */
static inline bool net_health_healthy(const net_health_input_t *in)
{
    if (!in || !in->wifi_connected) return false;
    if (!in->internet_ok) return false;
    if (!in->ws_connected || in->ws_rx_stale) return false;
    return true;
}

/* True while the ladder has nothing to supervise: the link is up, the probe
 * reached the internet and the clock has been set.  Only this resets the
 * escalation clock; the connection layer is net_task's business. */
static inline bool net_health_ladder_calm(const net_health_input_t *in)
{
    if (!in) return false;
    if (!in->wifi_connected) return false;
    if (!in->internet_ok) return false;
    if (!in->time_synced) return false;
    return true;
}

/* Rung that `down_ms` of continuous trouble has reached. */
static inline net_rung_t net_health_rung_for_down_ms(int64_t down_ms)
{
    if (down_ms >= NET_HEALTH_REBOOT_MS) return NET_RUNG_REBOOT;
    if (down_ms >= NET_HEALTH_DHCP_AGAIN_MS) return NET_RUNG_DHCP_AGAIN;
    if (down_ms >= NET_HEALTH_REASSOC_MS) return NET_RUNG_REASSOC;
    if (down_ms >= NET_HEALTH_DHCP_MS) return NET_RUNG_DHCP;
    if (down_ms >= NET_HEALTH_SNTP_MS) return NET_RUNG_SNTP;
    return NET_RUNG_NONE;
}

static inline net_action_t net_health_rung_action(net_rung_t rung)
{
    switch (rung) {
    case NET_RUNG_SNTP: return NET_ACTION_SNTP_RESTART;
    case NET_RUNG_DHCP:
    case NET_RUNG_DHCP_AGAIN: return NET_ACTION_DHCP_RENEW;
    case NET_RUNG_REASSOC: return NET_ACTION_WIFI_REASSOC;
    case NET_RUNG_REBOOT: return NET_ACTION_REBOOT;
    default: return NET_ACTION_NONE;
    }
}

/* Feed the current observation; returns the single action to perform now.
 * Keeps `s` consistent whether or not the caller acts on the answer. */
static inline net_action_t net_health_tick(net_health_state_t *s, const net_health_input_t *in,
                                           int64_t now_ms)
{
    if (!s || !in) return NET_ACTION_NONE;

    if (!in->supervise) {
        net_health_reset(s);
        return NET_ACTION_NONE;
    }

    bool calm = net_health_ladder_calm(in);
    if (calm) {
        /* Internet and clock are fine again: close the outage but keep
         * last_action_ms so a repair already in flight is not repeated. */
        s->down_since_ms = 0;
        s->rung = 0;
    }

    bool cooldown_elapsed = now_ms - s->last_action_ms >= NET_HEALTH_ACTION_COOLDOWN_MS;

    /* A socket that still claims to be connected but no longer delivers anything
     * is a half-open TCP session (the NAT mapping expired) or a silently dropped
     * gateway session.  net_task only acts on feishu_ws_connected() == false, so
     * this is the one connection failure the supervisor has to break open itself.
     * A socket that is plainly disconnected is left to net_task. */
    if (in->wifi_connected && in->ws_connected && in->ws_rx_stale && cooldown_elapsed) {
        s->last_action_ms = now_ms;
        s->last_action = NET_ACTION_WS_REFETCH;
        return NET_ACTION_WS_REFETCH;
    }

    /* Whatever is broken, it is not the internet path: net_task owns the socket
     * and a DHCP renewal would not help. */
    if (calm) return NET_ACTION_NONE;

    if (s->down_since_ms == 0) {
        s->down_since_ms = now_ms > 0 ? now_ms : 1;
        s->rung = 0;
        s->outages++;
    }

    /* Without an address nothing below can run: net_task is already reconnecting
     * Wi-Fi.  The clock keeps running so the outage age survives the link return. */
    if (!in->wifi_connected) return NET_ACTION_NONE;

    int64_t down_ms = now_ms - s->down_since_ms;
    if (down_ms < 0) down_ms = 0;

    if (!cooldown_elapsed) return NET_ACTION_NONE;

    net_rung_t want = net_health_rung_for_down_ms(down_ms);
    if (s->rung >= (uint32_t)want) return NET_ACTION_NONE;

    /* One rung per action: a long stretch without a decision (the link was down,
     * so no action could run) must not let one tick skip the cheap repairs. */
    if ((uint32_t)want > s->rung + 1) want = (net_rung_t)(s->rung + 1);

    if (in->internet_ok && !in->time_synced) {
        /* Reachable internet, unset clock: only rung 1 applies, and it is retried
         * instead of being spent once for the whole episode. */
        if (want > NET_RUNG_SNTP) want = NET_RUNG_SNTP;
        if ((uint32_t)want <= s->rung &&
            now_ms - s->last_action_ms >= NET_HEALTH_CLOCK_RETRY_MS) {
            want = NET_RUNG_SNTP;
            s->rung = (uint32_t)NET_RUNG_NONE;
        }
    }

    /* The restart rung is unavailable right now (cooldown, or a firmware operation
     * that is about to restart the device itself).  Leave the rung unspent so a
     * later tick -- once the block clears -- can still restart the device. */
    if (want == NET_RUNG_REBOOT && in->reboot_blocked) return NET_ACTION_NONE;

    s->rung = (uint32_t)want;
    s->last_action_ms = now_ms;
    s->last_action = net_health_rung_action(want);
    return s->last_action;
}
