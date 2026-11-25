#pragma once

#include <stdbool.h>

#include "cJSON.h"

/* Upstream health supervisor.
 *
 * The firmware used to have exactly one recovery habit: retry the Feishu long
 * connection every 5-8 s forever.  When the access point stayed reachable but
 * the upstream path (DNS, NAT mapping, DHCP lease, provider WAN) died, that habit
 * changed nothing and the device stayed dead until someone power cycled it -- and
 * every SMS received during those hours was lost.  This task watches the actual
 * internet reachability and escalates through cheap repairs before restarting.
 */
void net_health_start(void);

/* True when the last probe reached open.feishu.cn (DNS + TCP/443). */
bool net_health_internet_ok(void);

/* Appends {"internet_ok":..,"dns_ok":..,"tcp_ok":..,"down_ms":..,"outages":..,
 * "last_action":..,"auto_restarted":..} to `parent`.
 * `auto_restarted` means this boot was started by the supervisor itself. */
void net_health_status_json(cJSON *parent);
