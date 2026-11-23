#include "../feishu_proto.h"
#include "../feishu_event_dedup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    feishu_ws_assembler_t ws={0}; uint8_t *out=NULL; size_t out_len=0;
    if(feishu_ws_assembler_feed(&ws,0,6,"abc",3,false,&out,&out_len)!=0) return 1;
    if(feishu_ws_assembler_feed(&ws,3,6,"def",3,true,&out,&out_len)!=1 ||
       out_len!=6 || memcmp(out,"abcdef",6)) return 2;
    free(out); out=NULL;
    if(feishu_ws_assembler_feed(&ws,0,4,"ab",2,false,&out,&out_len)!=0) return 3;
    if(feishu_ws_assembler_feed(&ws,3,4,"d",1,true,&out,&out_len)!=-1) return 4;

    feishu_pbbp_assembler_t pbbp={0};
    if(feishu_pbbp_assembler_feed(&pbbp,"event-1",2,1,"world",5,10,&out,&out_len)!=0) return 5;
    if(feishu_pbbp_assembler_feed(&pbbp,"event-1",2,0,"hello ",6,11,&out,&out_len)!=1 ||
       out_len!=11 || memcmp(out,"hello world",11)) return 6;
    free(out); out=NULL;
    if(feishu_pbbp_assembler_feed(&pbbp,"event-2",2,0,"x",1,20,&out,&out_len)!=0) return 7;
    /* Expired partials are cleared and do not consume one of the bounded slots. */
    if(feishu_pbbp_assembler_feed(&pbbp,"event-3",1,0,"ok",2,60021,&out,&out_len)!=1 ||
       out_len!=2 || memcmp(out,"ok",2)) return 8;
    free(out);

    feishu_event_dedup_t dedup={0};
    if(feishu_event_dedup_contains(&dedup,"evt-a")) return 9;
    feishu_event_dedup_remember(&dedup,"evt-a");
    if(!feishu_event_dedup_contains(&dedup,"evt-a")) return 10;
    puts("PASS Feishu websocket/PBBP fragmentation and event dedup");
    return 0;
}
