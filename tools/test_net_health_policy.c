#include "../firmware/main/net_health_policy.h"

#include <stdio.h>

static int failures;

static void check(bool cond, const char *what)
{
    if (cond) {
        printf("[PASS] %s\n", what);
    } else {
        printf("[FAIL] %s\n", what);
        failures++;
    }
}

static net_health_input_t healthy_input(void)
{
    net_health_input_t in = {
        .supervise = true,
        .wifi_connected = true,
        .internet_ok = true,
        .ws_connected = true,
        .ws_rx_stale = false,
        .time_synced = true,
        .reboot_blocked = false,
    };
    return in;
}

/* Drive `in` for `duration_ms` and count the actions the ladder asks for. */
static int count_actions(net_health_state_t *st, const net_health_input_t *in,
                         int64_t start_ms, int64_t duration_ms, net_action_t *seen, int cap)
{
    int n = 0;
    for (int64_t dt = 0; dt <= duration_ms; dt += 5000) {
        net_action_t a = net_health_tick(st, in, start_ms + dt);
        if (a != NET_ACTION_NONE) {
            if (seen && n < cap) seen[n] = a;
            n++;
        }
    }
    return n;
}

/* First action the ladder asks for within `duration_ms`, or NET_ACTION_NONE. */
static net_action_t first_action(net_health_state_t *st, const net_health_input_t *in,
                                 int64_t start_ms, int64_t duration_ms)
{
    for (int64_t dt = 0; dt <= duration_ms; dt += 5000) {
        net_action_t a = net_health_tick(st, in, start_ms + dt);
        if (a != NET_ACTION_NONE) return a;
    }
    return NET_ACTION_NONE;
}

int main(void)
{
    net_health_state_t st;
    net_health_reset(&st);

    /* A healthy device is never disturbed. */
    net_health_input_t in = healthy_input();
    check(count_actions(&st, &in, 0, 60 * 60 * 1000, NULL, 0) == 0,
          "healthy device must not be touched");
    check(st.down_since_ms == 0, "healthy device keeps no outage");

    /* A device that is not configured yet has nothing to supervise: escalating
     * here would restart the box while the owner is still provisioning it. */
    net_health_reset(&st);
    in = healthy_input();
    in.supervise = false;
    in.internet_ok = false;
    in.ws_connected = false;
    in.wifi_connected = false;
    check(count_actions(&st, &in, 1000, 120 * 60 * 1000, NULL, 0) == 0,
          "unsupervised device never escalates");
    check(st.down_since_ms == 0 && st.outages == 0, "unsupervised device records nothing");

    /* A socket that still looks connected but went silent is the one connection
     * failure the supervisor has to break open itself. */
    net_health_reset(&st);
    in = healthy_input();
    in.ws_rx_stale = true;
    check(net_health_tick(&st, &in, 1000) == NET_ACTION_WS_REFETCH,
          "silent socket refetches the endpoint immediately");
    check(net_health_tick(&st, &in, 2000) == NET_ACTION_NONE, "refetch is rate limited");
    check(net_health_tick(&st, &in, 1000 + NET_HEALTH_ACTION_COOLDOWN_MS) ==
              NET_ACTION_WS_REFETCH,
          "refetch repeats once the cooldown expires");
    check(st.outages == 0 && st.down_since_ms == 0, "a refetch is not an internet outage");

    /* ws_rx_stale now comes from feishu_ws_alive() alone, so it is also true for a
     * plainly disconnected socket.  That socket is net_task's job, not ours. */
    net_health_reset(&st);
    in = healthy_input();
    in.ws_connected = false;
    in.ws_rx_stale = true;
    check(count_actions(&st, &in, 1000, 10 * 60 * 1000, NULL, 0) == 0,
          "a disconnected socket is left to the reconnect loop");

    /* P1-1: a long connection that is simply down on a working internet is
     * net_task's job.  It must not age the escalation clock, because otherwise the
     * first real internet failure lands with a 30-hour-old down_ms and restarts
     * the device on its very first tick. */
    net_health_reset(&st);
    in = healthy_input();
    in.ws_connected = false;
    int64_t ws_start = 5000;
    check(count_actions(&st, &in, ws_start, 2 * 60 * 60 * 1000, NULL, 0) == 0,
          "two hours of WS-only outage never escalate");
    check(st.down_since_ms == 0, "WS-only outage does not arm the escalation clock");
    check(st.rung == 0, "WS-only outage does not use a ladder rung");
    check(st.outages == 0, "WS-only outage is not counted as an internet outage");
    check(!net_health_healthy(&in), "a dead long connection is still reported unhealthy");
    check(net_health_ladder_calm(&in), "but the internet path itself is calm");

    /* P1-1: an internet outage after that long WS-only period must start from rung
     * 0 and must not restart the device on the first failed tick. */
    int64_t internet_fail = ws_start + 2 * 60 * 60 * 1000 + 5000;
    in.internet_ok = false;
    check(net_health_tick(&st, &in, internet_fail) == NET_ACTION_NONE,
          "the first tick of the real outage does nothing");
    check(st.outages == 1, "only the real outage is counted");
    check(st.rung == 0 && st.down_since_ms == internet_fail,
          "the escalation clock starts at the internet failure");
    check(first_action(&st, &in, internet_fail + 5000, 44 * 60 * 1000) ==
              NET_ACTION_SNTP_RESTART,
          "and the ladder starts at rung 1 instead of restarting the device");
    bool rebooted_early = false;
    for (int64_t dt = 0; dt <= 44 * 60 * 1000; dt += 5000) {
        if (net_health_tick(&st, &in, internet_fail + dt) == NET_ACTION_REBOOT) {
            rebooted_early = true;
        }
    }
    check(!rebooted_early, "no restart in the first 44 minutes of the real outage");

    /* Nothing works: the ladder must escalate in order, once per rung, and never
     * before its own threshold. */
    net_health_reset(&st);
    in = healthy_input();
    in.internet_ok = false;
    in.ws_connected = false;
    int64_t start = 10 * 60 * 1000; /* arbitrary non-zero epoch */
    net_action_t seen[8];
    int seen_n = count_actions(&st, &in, start, 50 * 60 * 1000, seen, 8);
    check(seen_n == 5, "one action per ladder rung");
    check(seen[0] == NET_ACTION_SNTP_RESTART, "rung 1 is SNTP");
    check(seen[1] == NET_ACTION_DHCP_RENEW, "rung 2 is DHCP");
    check(seen[2] == NET_ACTION_WIFI_REASSOC, "rung 3 is re-association");
    check(seen[3] == NET_ACTION_DHCP_RENEW, "rung 4 is DHCP again");
    check(seen[4] == NET_ACTION_REBOOT, "rung 5 is a controlled restart");
    check(st.outages == 1, "one outage counted");

    /* Recovery resets the ladder so a later outage starts from the cheap end, but
     * keeps the action cooldown: a repair that is still settling is not repeated. */
    int64_t last_action_ms = st.last_action_ms;
    in = healthy_input();
    check(net_health_tick(&st, &in, start + 60 * 60 * 1000) == NET_ACTION_NONE, "recovered");
    check(st.down_since_ms == 0 && st.rung == 0, "recovery resets the ladder");
    check(st.last_action_ms == last_action_ms, "recovery keeps the action cooldown");
    in.internet_ok = false;
    in.ws_connected = false;
    check(net_health_tick(&st, &in, start + 61 * 60 * 1000) == NET_ACTION_NONE,
          "a fresh outage starts below the first threshold");
    check(net_health_tick(&st, &in, start + 63 * 60 * 1000) == NET_ACTION_SNTP_RESTART,
          "and escalates from the bottom again");

    /* No address: the link layer owns recovery, the ladder stays quiet, but the
     * clock runs so the outage age survives the link coming back. */
    net_health_reset(&st);
    in = healthy_input();
    in.wifi_connected = false;
    in.internet_ok = false;
    in.ws_connected = false;
    check(count_actions(&st, &in, start, 50 * 60 * 1000, NULL, 0) == 0,
          "no address means no ladder action");
    check(st.outages == 1 && st.down_since_ms == start,
          "a link outage is still counted and timed");

    /* ...and when the link returns after 50 minutes with the internet still broken,
     * the cheap repairs must run first: one tick never skips a rung. */
    in.wifi_connected = true;
    check(net_health_tick(&st, &in, start + 50 * 60 * 1000 + 5000) ==
              NET_ACTION_SNTP_RESTART,
          "a long link outage does not jump straight to the restart");

    /* A clock that was never set breaks every TLS handshake even on a reachable
     * internet, so the ladder still has to run (and starts with SNTP). */
    net_health_reset(&st);
    in = healthy_input();
    in.time_synced = false;
    in.ws_connected = false;
    seen_n = count_actions(&st, &in, start, 5 * 60 * 1000, seen, 8);
    check(seen_n >= 1 && seen[0] == NET_ACTION_SNTP_RESTART,
          "an unsynced clock escalates to SNTP restart");

    /* Rung 1 also covers a clock that was never set while the connection layer is
     * fine: the internet answers, but without a clock every TLS handshake fails.
     * It cannot cover a clock that is set but drifted -- time_synced() is a latch
     * and the probe does not speak TLS (see net_health_policy.h). */
    net_health_reset(&st);
    in = healthy_input();
    in.time_synced = false;
    check(net_health_healthy(&in), "the connection layer may look fine");
    check(!net_health_ladder_calm(&in), "but an unset clock keeps the ladder armed");
    check(first_action(&st, &in, start, 5 * 60 * 1000) == NET_ACTION_SNTP_RESTART,
          "an unset clock restarts SNTP even with a reachable internet");

    /* An NTP-blocked network is a real case (some hotspots and corporate APs): the
     * internet answers, the clock never syncs.  The ladder must stay at rung 1 and
     * retry SNTP instead of walking DHCP -> reassoc -> restart every hour. */
    net_health_reset(&st);
    in = healthy_input();
    in.time_synced = false;
    bool escalated = false;
    int sntp_actions = 0;
    for (int64_t dt = 0; dt <= 6 * 60 * 60 * 1000; dt += 5000) {
        net_action_t a = net_health_tick(&st, &in, start + dt);
        if (a == NET_ACTION_SNTP_RESTART) sntp_actions++;
        else if (a != NET_ACTION_NONE) escalated = true;
    }
    check(!escalated, "a reachable internet with an unset clock never escalates past SNTP");
    check(st.rung <= NET_RUNG_SNTP, "the clock-only ladder stays at rung 1");
    check(sntp_actions >= 2, "SNTP is retried periodically while the clock stays unset");

    /* While the restart rung is blocked (the 2 h cooldown after our own restart, or
     * a firmware operation in flight) the ladder must not restart again: a genuine
     * multi-hour provider outage must not turn into a reboot loop.  The rung stays
     * unspent, so the restart happens once the block clears. */
    net_health_reset(&st);
    in = healthy_input();
    in.internet_ok = false;
    in.ws_connected = false;
    in.reboot_blocked = true;
    bool rebooted = false;
    for (int64_t dt = 0; dt <= 120 * 60 * 1000; dt += 5000) {
        if (net_health_tick(&st, &in, start + dt) == NET_ACTION_REBOOT) rebooted = true;
    }
    check(!rebooted, "a blocked restart rung is not used");
    check(st.rung == NET_RUNG_DHCP_AGAIN, "the blocked restart leaves its rung unspent");
    in.reboot_blocked = false;
    check(net_health_tick(&st, &in, start + 120 * 60 * 1000 + 5000) == NET_ACTION_REBOOT,
          "the restart happens once the block clears");

    if (failures) {
        printf("[SUMMARY] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[PASS] network health policy tests\n");
    return 0;
}
