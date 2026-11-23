#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Retry only transport failures before an HTTP response or server 5xx. */
static inline bool feishu_retry_policy(bool transport_failure, int http_status)
{
    return http_status >= 500 || (transport_failure && http_status == 0);
}

/* Reply message IDs are the event's stable idempotency key across retries. */
static inline bool feishu_reply_uuid(const char *message_id, char out[51])
{
    if (!message_id || !out) return false;
    size_t n=strlen(message_id);
    if (!n || n>256) return false;
    for (size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)message_id[i];
        if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
              (c>='0'&&c<='9') || c=='_' || c=='-')) return false;
    }
    if(n<=44) return snprintf(out,51,"reply_%s",message_id)==(int)(n+6);
    unsigned long long hash_a=14695981039346656037ULL;
    unsigned long long hash_b=7809847782465536322ULL;
    for(size_t i=0;i<n;i++) {
        hash_a^=(unsigned char)message_id[i];
        hash_a*=1099511628211ULL;
        hash_b^=(unsigned char)message_id[i];
        hash_b*=14029467366897019727ULL;
    }
    return snprintf(out,51,"reply_h%016llx%016llx",hash_a,hash_b)==39;
}
