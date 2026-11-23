#include "feishu_proto.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void feishu_ws_assembler_reset(feishu_ws_assembler_t *a)
{
    if (!a) return;
    free(a->data);
    memset(a,0,sizeof(*a));
}

int feishu_ws_assembler_feed(feishu_ws_assembler_t *a, size_t offset, size_t total,
                             const void *chunk, size_t chunk_len, bool fin,
                             uint8_t **out, size_t *out_len)
{
    if (out) *out=NULL;
    if (out_len) *out_len=0;
    if (!a || !out || !out_len || (!chunk && chunk_len) || total==0 ||
        total>FEISHU_WS_MESSAGE_MAX || offset>total || chunk_len>total-offset) {
        if (a) feishu_ws_assembler_reset(a);
        return -1;
    }
    if (offset==0) {
        feishu_ws_assembler_reset(a);
        a->data=malloc(total);
        if (!a->data) return -1;
        a->total=total;
    }
    if (!a->data || total!=a->total || offset!=a->received) {
        feishu_ws_assembler_reset(a);
        return -1;
    }
    if (chunk_len) memcpy(a->data+a->received,chunk,chunk_len);
    a->received+=chunk_len;
    if (fin && a->received!=a->total) { feishu_ws_assembler_reset(a); return -1; }
    if (!fin || a->received<a->total) return 0;
    *out=a->data; *out_len=a->total;
    a->data=NULL; a->total=0; a->received=0;
    return 1;
}

static void clear_slot(feishu_pbbp_slot_t *s)
{
    for (size_t i=0;i<FEISHU_PBBP_PARTS_MAX;i++) free(s->part[i]);
    memset(s,0,sizeof(*s));
}

void feishu_pbbp_assembler_reset(feishu_pbbp_assembler_t *a)
{
    if (!a) return;
    for(size_t i=0;i<FEISHU_PBBP_SLOTS;i++) clear_slot(&a->slots[i]);
}

int feishu_pbbp_assembler_feed(feishu_pbbp_assembler_t *a, const char *message_id,
                               int sum, int seq, const void *part, size_t part_len,
                               int64_t now_ms, uint8_t **out, size_t *out_len)
{
    if(out) *out=NULL;
    if(out_len) *out_len=0;
    if(!a || !out || !out_len || !message_id || !message_id[0] ||
       sum<1 || sum>FEISHU_PBBP_PARTS_MAX || seq<0 || seq>=sum ||
       (!part && part_len) || part_len>FEISHU_WS_MESSAGE_MAX) return -1;
    feishu_pbbp_slot_t *s=NULL;
    for(size_t i=0;i<FEISHU_PBBP_SLOTS;i++) {
        feishu_pbbp_slot_t *candidate=&a->slots[i];
        if(candidate->id[0] && now_ms-candidate->touched_ms>60000) clear_slot(candidate);
        if(candidate->id[0] && strcmp(candidate->id,message_id)==0) s=candidate;
    }
    if(!s) for(size_t i=0;i<FEISHU_PBBP_SLOTS;i++) if(!a->slots[i].id[0]) { s=&a->slots[i]; break; }
    if(!s) return -1;
    if(!s->id[0]) { snprintf(s->id,sizeof(s->id),"%s",message_id); s->sum=sum; }
    if(s->sum!=sum) { clear_slot(s); return -1; }
    s->touched_ms=now_ms;
    if(!s->got[seq]) {
        size_t total=part_len;
        for(int i=0;i<s->sum;i++) total+=s->len[i];
        if(total>FEISHU_WS_MESSAGE_MAX) { clear_slot(s); return -1; }
        s->part[seq]=malloc(part_len ? part_len : 1);
        if(!s->part[seq]) return -1;
        if(part_len) memcpy(s->part[seq],part,part_len);
        s->len[seq]=part_len; s->got[seq]=true;
    }
    size_t total=0;
    for(int i=0;i<s->sum;i++) {
        if(!s->got[i]) return 0;
        total+=s->len[i];
    }
    uint8_t *joined=malloc(total ? total : 1);
    if(!joined) { clear_slot(s); return -1; }
    size_t pos=0;
    for(int i=0;i<s->sum;i++) { memcpy(joined+pos,s->part[i],s->len[i]); pos+=s->len[i]; }
    *out=joined; *out_len=total;
    clear_slot(s);
    return 1;
}
