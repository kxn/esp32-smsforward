#pragma once

#include <stdbool.h>

/* Feishu answers HTTP 200 with a non-zero `code` for most logical failures, so a
 * status-code-only check happily keeps using a tenant_access_token that the
 * server already invalidated.  A send then fails silently for up to the token's
 * own lifetime (~2 h) even though the network is perfectly fine. */

#define FEISHU_TOKEN_CODE_INVALID 99991661  /* invalid access token */
#define FEISHU_TOKEN_CODE_EXPIRED 99991663  /* token expired */
#define FEISHU_TOKEN_CODE_APP_MISMATCH 99991668

static inline bool feishu_code_is_token_stale(long code)
{
    return code == FEISHU_TOKEN_CODE_INVALID || code == FEISHU_TOKEN_CODE_EXPIRED ||
           code == FEISHU_TOKEN_CODE_APP_MISMATCH;
}

/* One forced refresh per delivery attempt: retrying more inside a single SMS
 * delivery would stall the SMS worker behind repeated 10 s HTTP timeouts. */
static inline bool feishu_should_retry_after_reject(bool already_retried, long code, int http_status)
{
    if (already_retried) return false;
    return feishu_code_is_token_stale(code) || http_status == 401;
}
