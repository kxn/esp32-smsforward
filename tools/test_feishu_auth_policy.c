#include "../firmware/main/feishu_auth_policy.h"

int main(void)
{
    /* The three code values Feishu uses when the tenant token is no longer usable. */
    if (!feishu_code_is_token_stale(99991661)) return 1;
    if (!feishu_code_is_token_stale(99991663)) return 2;
    if (!feishu_code_is_token_stale(99991668)) return 3;
    if (feishu_code_is_token_stale(0)) return 4;
    if (feishu_code_is_token_stale(230002)) return 5;

    /* A rejected token is retried exactly once, with a freshly fetched token. */
    if (!feishu_should_retry_after_reject(false, 99991663, 200)) return 6;
    if (!feishu_should_retry_after_reject(false, 0, 401)) return 7;
    if (feishu_should_retry_after_reject(true, 99991663, 200)) return 8;
    /* A real content error must not burn a second request. */
    if (feishu_should_retry_after_reject(false, 230002, 200)) return 9;
    if (feishu_should_retry_after_reject(false, 0, 500)) return 10;
    return 0;
}
