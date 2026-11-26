#pragma once

/* Online mark lookup for incoming calls.
 *
 * A "mark" is a label an online service has attached to a phone number (快递,
 * 外卖, 骚扰, 诈骗...).  The device can only learn those from a third-party HTTP
 * service, so this is opt-in: the owner pastes a request URL template and, when
 * the service needs one, an API key in the LAN console.  Without a configured
 * template nothing is ever queried and call notices are unchanged.
 *
 * The lookup is deliberately short: it runs before the call notice is sent and
 * gives up after the configured timeout (default 2 s), so a slow or dead service
 * costs a couple of seconds at worst and never a lost notice.  Results are
 * cached in RAM, so repeated calls from the same number are instant. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Create the internal lock; call once during boot before any lookup. */
void peer_mark_init(void);

/* True when the owner configured a request URL template. */
bool peer_mark_enabled(void);

/* Bounded lookup for an incoming call.  Returns a NUL-terminated label owned by
 * this module (valid until the next call to this function), or NULL when the
 * lookup is disabled, already cached as unmarked, has no configured template,
 * timed out, or failed.  Callers must not free the result. */
const char *peer_mark_lookup(const char *number);

/* Diagnostics for the status RPC. */
unsigned peer_mark_queries(void);
unsigned peer_mark_timeouts(void);
unsigned peer_mark_failures(void);
unsigned peer_mark_cache_entries(void);
uint32_t peer_mark_last_ms(void);
unsigned peer_mark_last_bytes(void);
const char *peer_mark_last_label(void);
