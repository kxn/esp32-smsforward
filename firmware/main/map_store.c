#include "map_store.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define MAP_EXT_RAM EXT_RAM_BSS_ATTR
#else
#define MAP_EXT_RAM
#endif

typedef struct {
    char msg_id[48];
    int slot;
    char peer[32];
    char record_id[40];
} map_entry_t;

typedef struct {
    char magic[4];
    uint32_t version;
    uint64_t seq;
    map_entry_t entry;
    uint32_t checksum;
} map_disk_record_t;

/* Keep identity bindings in a sidecar so existing map.dat records remain readable. */
typedef struct {
    char msg_id[48];
    char iccid[24];
} map_identity_t;

typedef struct {
    char magic[4];
    uint32_t version;
    uint64_t seq;
    map_identity_t entry;
    uint32_t checksum;
} map_identity_record_t;

/* Write-failure streak. Flash/FAT writes can fail transiently (power glitch,
 * busy volume), so a failure must not disable the store for the rest of the
 * boot. After MAP_STORE_FAIL_LATCH consecutive failures the store stops
 * touching the filesystem and only probes with one write every
 * MAP_STORE_RETRY_EVERY attempted writes; a probe that succeeds clears the
 * streak. This needs no clock and keeps the fast path allocation-free. */
typedef struct {
    unsigned fails;
    unsigned skips;
} write_gate_t;

static map_entry_t s_map[MAP_STORE_CAPACITY] MAP_EXT_RAM;
static map_disk_record_t s_disk[MAP_STORE_CAPACITY + 1] MAP_EXT_RAM;
static char s_path[128];
static char s_identity_path[128];
static size_t s_head;
static uint64_t s_seq;
static write_gate_t s_write_gate;
static map_identity_t s_identity[MAP_STORE_CAPACITY] MAP_EXT_RAM;
static map_identity_record_t s_identity_disk[MAP_STORE_CAPACITY + 1] MAP_EXT_RAM;
static size_t s_identity_head;
static uint64_t s_identity_seq;
static write_gate_t s_identity_write_gate;

static void copy_string(char *dst, size_t len, const char *src)
{
    if (len) snprintf(dst, len, "%s", src ? src : "");
}

bool map_store_find_message(const char *record_id, char *message_id, size_t capacity)
{
    if (!record_id || !*record_id || !message_id || !capacity) return false;
    for (size_t i = 0; i < MAP_STORE_CAPACITY; ++i) {
        size_t at = (s_head + MAP_STORE_CAPACITY - 1 - i) % MAP_STORE_CAPACITY;
        if (!strcmp(s_map[at].record_id, record_id) && s_map[at].msg_id[0]) {
            if (strlen(s_map[at].msg_id) >= capacity) return false;
            copy_string(message_id, capacity, s_map[at].msg_id);
            return true;
        }
    }
    return false;
}

bool map_store_identity_path(const char *path, char *out, size_t out_len)
{
    if(!path || !out || !out_len) return false;
    const char *slash1=strrchr(path,'/');
    const char *slash2=strrchr(path,'\\');
    const char *slash=slash1;
    if(!slash || (slash2 && slash2>slash)) slash=slash2;
    const char *dot=strrchr(path,'.');
    if(!dot || (slash && dot<slash) || strcmp(dot,".dat")!=0) return false;
    size_t prefix=(size_t)(dot-path);
    if(prefix+sizeof(".sim")>out_len) return false;
    memcpy(out,path,prefix);
    memcpy(out+prefix,".sim",sizeof(".sim"));
    return true;
}

static uint32_t checksum(const map_disk_record_t *r)
{
    const uint8_t *p=(const uint8_t *)r;
    size_t n=offsetof(map_disk_record_t,checksum);
    uint32_t h=2166136261u;
    for(size_t i=0;i<n;i++) h=(h^p[i])*16777619u;
    return h;
}

static uint32_t identity_checksum(const map_identity_record_t *r)
{
    const uint8_t *p=(const uint8_t *)r;
    size_t n=offsetof(map_identity_record_t,checksum);
    uint32_t h=2166136261u;
    for(size_t i=0;i<n;i++) h=(h^p[i])*16777619u;
    return h;
}

static bool flush_record(FILE *fp, size_t slot, const map_disk_record_t *r)
{
    long offset=(long)(slot*sizeof(*r));
    if(fseek(fp,offset,SEEK_SET)!=0 || fwrite(r,1,sizeof(*r),fp)!=sizeof(*r) || fflush(fp)!=0) return false;
    return fsync(fileno(fp))==0;
}

static bool gate_short_circuited(const write_gate_t *g)
{
    return g->fails>=MAP_STORE_FAIL_LATCH;
}

/* True when this call may touch the filesystem. While short-circuited only the
 * one probe that lands on every MAP_STORE_RETRY_EVERY-th attempted write is
 * let through, which bounds flash churn and retries a stale fault quickly. */
static bool gate_allows_write(write_gate_t *g)
{
    if(!gate_short_circuited(g)) return true;
    if(++g->skips<MAP_STORE_RETRY_EVERY) return false;
    g->skips=0;
    return true;
}

static void gate_note_result(write_gate_t *g, bool ok)
{
    g->skips=0;
    if(ok) { g->fails=0; return; }
    if(g->fails<MAP_STORE_FAIL_LATCH) ++g->fails;
}

/* Diagnostics: false while a write streak is short-circuiting new writes. A
 * short-circuited store still serves lookups from memory and heals itself on
 * the next successful probe. */
bool map_store_healthy(void)
{
    return !gate_short_circuited(&s_write_gate) && !gate_short_circuited(&s_identity_write_gate);
}

bool map_store_init(const char *path)
{
    if(!path || !path[0] || strlen(path)>=sizeof(s_path) ||
       !map_store_identity_path(path,s_identity_path,sizeof(s_identity_path))) return false;
    copy_string(s_path,sizeof(s_path),path);
    memset(s_map,0,sizeof(s_map)); memset(s_disk,0,sizeof(s_disk));
    s_head=0; s_seq=0; memset(&s_write_gate,0,sizeof(s_write_gate));
    memset(s_identity,0,sizeof(s_identity)); memset(s_identity_disk,0,sizeof(s_identity_disk));
    s_identity_head=0; s_identity_seq=0; memset(&s_identity_write_gate,0,sizeof(s_identity_write_gate));
    FILE *fp=fopen(s_path,"rb");
    if(fp) {
        for(size_t i=0;i<MAP_STORE_CAPACITY+1;i++) {
            map_disk_record_t r;
            if(fseek(fp,(long)(i*sizeof(r)),SEEK_SET)!=0 || fread(&r,1,sizeof(r),fp)!=sizeof(r)) break;
            if(memcmp(r.magic,"MPR1",4)==0 && r.version==1 && r.seq>0 && r.entry.msg_id[0] &&
               r.entry.slot>=0 && r.entry.slot<2 && r.checksum==checksum(&r)) s_disk[i]=r;
        }
        fclose(fp);
    }
    uint64_t last=0; size_t count=0;
    for(size_t n=0;n<MAP_STORE_CAPACITY;n++) {
        size_t best=MAP_STORE_CAPACITY+1; uint64_t best_seq=UINT64_MAX;
        for(size_t i=0;i<MAP_STORE_CAPACITY+1;i++) {
            if(s_disk[i].seq>last && s_disk[i].seq<best_seq) { best=i; best_seq=s_disk[i].seq; }
        }
        if(best==MAP_STORE_CAPACITY+1) break;
        s_map[count++]=s_disk[best].entry; last=best_seq;
    }
    s_seq=last; s_head=count%MAP_STORE_CAPACITY;

    fp=fopen(s_identity_path,"rb");
    if(fp) {
        for(size_t i=0;i<MAP_STORE_CAPACITY+1;i++) {
            map_identity_record_t r;
            if(fseek(fp,(long)(i*sizeof(r)),SEEK_SET)!=0 || fread(&r,1,sizeof(r),fp)!=sizeof(r)) break;
            if(memcmp(r.magic,"MSI1",4)==0 && r.version==1 && r.seq>0 && r.entry.msg_id[0] &&
               r.checksum==identity_checksum(&r)) s_identity_disk[i]=r;
        }
        fclose(fp);
    }
    last=0; count=0;
    for(size_t n=0;n<MAP_STORE_CAPACITY;n++) {
        size_t best=MAP_STORE_CAPACITY+1; uint64_t best_seq=UINT64_MAX;
        for(size_t i=0;i<MAP_STORE_CAPACITY+1;i++) {
            if(s_identity_disk[i].seq>last && s_identity_disk[i].seq<best_seq) { best=i; best_seq=s_identity_disk[i].seq; }
        }
        if(best==MAP_STORE_CAPACITY+1) break;
        s_identity[count++]=s_identity_disk[best].entry; last=best_seq;
    }
    s_identity_seq=last; s_identity_head=count%MAP_STORE_CAPACITY;
    return true;
}

bool map_store_note(const char *msg_id, int slot, const char *peer, const char *record_id)
{
    if(!s_path[0] || !msg_id || !msg_id[0] || slot<0 || slot>=2 || !peer || !peer[0]) return false;
    if(!gate_allows_write(&s_write_gate)) return false;
    FILE *fp=fopen(s_path,"r+b");
    if(!fp) fp=fopen(s_path,"w+b");
    if(!fp) { gate_note_result(&s_write_gate,false); return false; }
    size_t target=MAP_STORE_CAPACITY; uint64_t oldest=UINT64_MAX;
    for(size_t i=0;i<MAP_STORE_CAPACITY;i++) {
        if(memcmp(s_disk[i].magic,"MPR1",4)!=0) { target=i; break; }
        if(s_disk[i].seq<oldest) { oldest=s_disk[i].seq; target=i; }
    }
    map_disk_record_t r={0};
    memcpy(r.magic,"MPR1",4); r.version=1; r.seq=s_seq+1;
    copy_string(r.entry.msg_id,sizeof(r.entry.msg_id),msg_id);
    r.entry.slot=slot; copy_string(r.entry.peer,sizeof(r.entry.peer),peer);
    copy_string(r.entry.record_id,sizeof(r.entry.record_id),record_id);
    r.checksum=checksum(&r);
    long journal_offset=(long)(MAP_STORE_CAPACITY*sizeof(r));
    bool ok=fseek(fp,journal_offset,SEEK_SET)==0 && fwrite(&r,1,sizeof(r),fp)==sizeof(r) &&
            fflush(fp)==0 && fsync(fileno(fp))==0;
    if(ok) ok=flush_record(fp,target,&r);
    if(fclose(fp)!=0) ok=false;
    if(!ok) { gate_note_result(&s_write_gate,false); return false; }
    gate_note_result(&s_write_gate,true);
    s_disk[MAP_STORE_CAPACITY]=r; s_disk[target]=r; s_seq=r.seq;
    s_map[s_head]=r.entry; s_head=(s_head+1)%MAP_STORE_CAPACITY;
    return true;
}

static bool identity_note(const char *msg_id, const char *iccid)
{
    if(!s_identity_path[0] || !msg_id || !msg_id[0]) return false;
    if(!gate_allows_write(&s_identity_write_gate)) return false;
    FILE *fp=fopen(s_identity_path,"r+b");
    if(!fp) fp=fopen(s_identity_path,"w+b");
    if(!fp) { gate_note_result(&s_identity_write_gate,false); return false; }
    size_t target=MAP_STORE_CAPACITY; uint64_t oldest=UINT64_MAX;
    for(size_t i=0;i<MAP_STORE_CAPACITY;i++) {
        if(memcmp(s_identity_disk[i].magic,"MSI1",4)!=0) { target=i; break; }
        if(s_identity_disk[i].seq<oldest) { oldest=s_identity_disk[i].seq; target=i; }
    }
    map_identity_record_t r={0};
    memcpy(r.magic,"MSI1",4); r.version=1; r.seq=s_identity_seq+1;
    copy_string(r.entry.msg_id,sizeof(r.entry.msg_id),msg_id);
    copy_string(r.entry.iccid,sizeof(r.entry.iccid),iccid);
    r.checksum=identity_checksum(&r);
    long journal_offset=(long)(MAP_STORE_CAPACITY*sizeof(r));
    bool ok=fseek(fp,journal_offset,SEEK_SET)==0 && fwrite(&r,1,sizeof(r),fp)==sizeof(r) &&
            fflush(fp)==0 && fsync(fileno(fp))==0;
    if(ok) {
        long offset=(long)(target*sizeof(r));
        ok=fseek(fp,offset,SEEK_SET)==0 && fwrite(&r,1,sizeof(r),fp)==sizeof(r) &&
           fflush(fp)==0 && fsync(fileno(fp))==0;
    }
    if(fclose(fp)!=0) ok=false;
    if(!ok) { gate_note_result(&s_identity_write_gate,false); return false; }
    gate_note_result(&s_identity_write_gate,true);
    s_identity_disk[MAP_STORE_CAPACITY]=r; s_identity_disk[target]=r; s_identity_seq=r.seq;
    s_identity[s_identity_head]=r.entry; s_identity_head=(s_identity_head+1)%MAP_STORE_CAPACITY;
    return true;
}

bool map_store_note_sim(const char *msg_id, int slot, const char *peer,
                        const char *record_id, const char *iccid)
{
    if(!iccid || strlen(iccid)<10 || strlen(iccid)>=24) return false;
    if(!map_store_note(msg_id,slot,peer,record_id)) return false;
    return identity_note(msg_id,iccid);
}

bool map_store_lookup(const char *msg_id, int *slot, char *peer, size_t peer_len,
                      char *record_id, size_t record_id_len)
{
    if(!msg_id || !msg_id[0]) return false;
    for(size_t i=0;i<MAP_STORE_CAPACITY;i++) {
        if(s_map[i].msg_id[0] && strcmp(s_map[i].msg_id,msg_id)==0) {
            if(slot) *slot=s_map[i].slot;
            copy_string(peer,peer_len,s_map[i].peer);
            copy_string(record_id,record_id_len,s_map[i].record_id);
            return true;
        }
    }
    return false;
}

bool map_store_lookup_sim(const char *msg_id, int *slot, char *peer, size_t peer_len,
                          char *record_id, size_t record_id_len,
                          char *iccid, size_t iccid_len)
{
    if(!map_store_lookup(msg_id,slot,peer,peer_len,record_id,record_id_len)) return false;
    copy_string(iccid,iccid_len,"");
    for(size_t i=0;i<MAP_STORE_CAPACITY;i++) {
        if(s_identity[i].msg_id[0] && strcmp(s_identity[i].msg_id,msg_id)==0) {
            copy_string(iccid,iccid_len,s_identity[i].iccid);
            break;
        }
    }
    return true;
}

bool map_store_clear(void)
{
    if (s_path[0] && unlink(s_path) != 0 && errno != ENOENT) return false;
    if (s_identity_path[0] && unlink(s_identity_path) != 0 && errno != ENOENT) return false;
    memset(s_map, 0, sizeof(s_map));
    memset(s_disk, 0, sizeof(s_disk));
    s_head = 0; s_seq = 0; memset(&s_write_gate,0,sizeof(s_write_gate));
    memset(s_identity,0,sizeof(s_identity)); memset(s_identity_disk,0,sizeof(s_identity_disk));
    s_identity_head=0; s_identity_seq=0; memset(&s_identity_write_gate,0,sizeof(s_identity_write_gate));
    return true;
}
