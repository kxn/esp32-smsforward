#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FEISHU_EVENT_DEDUP_SIZE 64
typedef struct { char ids[FEISHU_EVENT_DEDUP_SIZE][80]; unsigned next; } feishu_event_dedup_t;

static inline bool feishu_event_dedup_contains(const feishu_event_dedup_t *cache, const char *id)
{
    if (!cache || !id || !id[0]) return false;
    for (unsigned i=0;i<FEISHU_EVENT_DEDUP_SIZE;i++) {
        if (cache->ids[i][0] && strcmp(cache->ids[i],id)==0) return true;
    }
    return false;
}

static inline void feishu_event_dedup_remember(feishu_event_dedup_t *cache, const char *id)
{
    if (!cache || !id || !id[0]) return;
    snprintf(cache->ids[cache->next++ % FEISHU_EVENT_DEDUP_SIZE],
             sizeof(cache->ids[0]),"%s",id);
}

static inline bool feishu_event_dedup_seen(feishu_event_dedup_t *cache, const char *id)
{
    bool seen=feishu_event_dedup_contains(cache,id);
    if(!seen) feishu_event_dedup_remember(cache,id);
    return seen;
}
