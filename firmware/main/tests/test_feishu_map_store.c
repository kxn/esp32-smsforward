#include "../map_store.h"
#include "../sim_identity_policy.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void make_id(char *out, size_t len, int i)
{
    snprintf(out,len,"msg_%03d",i);
}

int main(void)
{
    const char *path="feishu_map_test.dat";
    char sidecar_path[64];
    if(!map_store_identity_path("/data/map.dat",sidecar_path,sizeof(sidecar_path)) ||
       strcmp(sidecar_path,"/data/map.sim")!=0) return 21;
    remove(path);
    map_store_identity_path(path,sidecar_path,sizeof(sidecar_path));
    remove(sidecar_path);
    if(!map_store_init(path)) return 1;
    char id[48], peer[32], record[40], iccid[24]="";
    int slot=-1;
    for(int i=0;i<MAP_STORE_CAPACITY;i++) {
        make_id(id,sizeof(id),i);
        if(!map_store_note(id,i%2,"+8613800000000","sms_test")) return 2;
    }
    if(!map_store_lookup("msg_000",&slot,peer,sizeof(peer),record,sizeof(record)) ||
       slot!=0 || strcmp(peer,"+8613800000000") || strcmp(record,"sms_test")) return 3;
    if(!map_store_note("msg_512",1,"+8613900000000","sms_wrap")) return 4;
    if(map_store_lookup("msg_000",NULL,NULL,0,NULL,0)) return 5;
    if(!map_store_lookup("msg_001",&slot,peer,sizeof(peer),record,sizeof(record)) || slot!=1) return 6;
    if(!map_store_lookup("msg_512",&slot,peer,sizeof(peer),record,sizeof(record)) ||
       slot!=1 || strcmp(record,"sms_wrap")) return 7;

    /* Old map records remain readable but have no identity and must fail closed on quote. */
    if(!map_store_lookup_sim("msg_001",&slot,peer,sizeof(peer),record,sizeof(record),iccid,sizeof(iccid)) || iccid[0]) return 8;
    if(!map_store_init(path)) return 10;
    if(map_store_lookup("msg_000",NULL,NULL,0,NULL,0)) return 11;
    if(!map_store_lookup("msg_001",&slot,peer,sizeof(peer),record,sizeof(record)) || slot!=1) return 12;
    if(!map_store_lookup("msg_512",&slot,peer,sizeof(peer),record,sizeof(record)) ||
       slot!=1 || strcmp(peer,"+8613900000000") || strcmp(record,"sms_wrap")) return 13;
    if(!map_store_note_sim("bound_1",0,"+8613800000000","sms_bound",
                           "89860123456789012345")) return 9;
    if(!map_store_init(path)) return 20;
    if(!map_store_lookup_sim("bound_1",&slot,peer,sizeof(peer),record,sizeof(record),iccid,sizeof(iccid)) ||
       slot!=0 || strcmp(iccid,"89860123456789012345")) return 14;

    const char *swapped[2]={"89860111111111111111","89860123456789012345"};
    const bool readable[2]={true,true};
    int current_slot=-1;
    if(sim_identity_match(iccid,swapped,readable,&current_slot)!=SIM_IDENTITY_UNIQUE || current_slot!=1) return 15;
    const char *same[2]={"89860123456789012345","89860123456789012345"};
    if(sim_identity_match(iccid,same,readable,&current_slot)!=SIM_IDENTITY_AMBIGUOUS) return 16;
    const bool one_unreadable[2]={true,false};
    if(sim_identity_match(iccid,swapped,one_unreadable,&current_slot)!=SIM_IDENTITY_UNREADABLE) return 17;
    const char *absent[2]={"89860111111111111111","89860122222222222222"};
    if(sim_identity_match(iccid,absent,readable,&current_slot)!=SIM_IDENTITY_NO_MATCH) return 18;

    if(!map_store_clear()) return 19;
    remove(path);
    remove(sidecar_path);
    puts("PASS Feishu map wrap/reload, SIM identity binding, legacy invalidation, and safe slot matching");
    return 0;
}
