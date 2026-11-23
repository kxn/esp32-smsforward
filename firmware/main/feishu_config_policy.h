#pragma once

#include <stdbool.h>
#include <string.h>

/* The first boot-time load is restoration, not an account rotation. */
static inline bool feishu_credentials_changed(bool initialized,
                                              const char *old_app_id,
                                              const char *old_app_secret,
                                              const char *new_app_id,
                                              const char *new_app_secret)
{
    return initialized &&
           (strcmp(old_app_id, new_app_id) != 0 ||
            strcmp(old_app_secret, new_app_secret) != 0);
}
