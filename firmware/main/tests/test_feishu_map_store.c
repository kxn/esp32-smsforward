#include "../map_store.h"
#include "../sim_identity_policy.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

    /* Regression: a transient write failure must not latch for the rest of the
     * boot. A missing directory makes every write fail while the store is
     * otherwise healthy, and creating it later heals the same store instance. */
    const char *blocked_path="map_blocked_dir/map.dat";
    char blocked_sidecar[64];
    (void)unlink(blocked_path);
    if(!map_store_identity_path(blocked_path,blocked_sidecar,sizeof(blocked_sidecar))) return 22;
    (void)unlink(blocked_sidecar);
    (void)rmdir("map_blocked_dir");
    if(!map_store_init(blocked_path)) return 23;
    if(!map_store_healthy()) return 24;
    for(int i=0;i<MAP_STORE_FAIL_LATCH;i++) {
        make_id(id,sizeof(id),i);
        if(map_store_note(id,0,"+8613800000000","sms_blocked")) return 25;
    }
    if(map_store_healthy()) return 26;

    /* Short-circuited calls are rejected before any filesystem access. */
    if(mkdir("map_blocked_dir",0700)!=0) return 27;
    for(int i=0;i<MAP_STORE_RETRY_EVERY-1;i++) {
        make_id(id,sizeof(id),100+i);
        if(map_store_note(id,0,"+8613800000000","sms_blocked")) return 28;
    }
    if(access(blocked_path,F_OK)==0) return 29;
    if(map_store_healthy()) return 30;

    /* The MAP_STORE_RETRY_EVERY-th attempt after the latch is the probe, and a
     * successful probe re-enables normal writes without a reinit. */
    if(!map_store_note("msg_recovered",0,"+8613800000000","sms_recovered")) return 31;
    if(!map_store_healthy()) return 32;
    if(!map_store_lookup("msg_recovered",&slot,peer,sizeof(peer),record,sizeof(record)) ||
       strcmp(record,"sms_recovered")) return 33;
    if(!map_store_init(blocked_path)) return 34;
    if(!map_store_lookup("msg_recovered",&slot,peer,sizeof(peer),record,sizeof(record)) ||
       strcmp(record,"sms_recovered")) return 35;
    if(!map_store_healthy()) return 36;

    /* A single failure stays below the latch and recovers on the next write. */
    (void)unlink(blocked_path);
    (void)unlink(blocked_sidecar);
    if(rmdir("map_blocked_dir")!=0) return 37;
    if(map_store_note("msg_transient",0,"+8613800000000","sms_transient")) return 38;
    if(!map_store_healthy()) return 39;
    if(mkdir("map_blocked_dir",0700)!=0) return 40;
    if(!map_store_note("msg_healed",0,"+8613800000000","sms_healed")) return 41;
    if(!map_store_lookup("msg_healed",&slot,peer,sizeof(peer),record,sizeof(record)) ||
       strcmp(record,"sms_healed")) return 42;

    if(!map_store_clear()) return 19;
    (void)rmdir("map_blocked_dir");
    remove(path);
    remove(sidecar_path);
    puts("PASS Feishu map wrap/reload, SIM identity binding, legacy invalidation, safe slot matching, and write-fault recovery");
    return 0;
}
