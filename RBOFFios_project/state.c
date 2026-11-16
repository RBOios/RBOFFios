/* Quick save / quick load of the whole emulated machine (one slot = one file).
 *
 * What a state is: the 512 MB guest address space (only the regions the emulator ever uses, zero pages skipped), the CPU and every guest
 * thread, and the emulator-side bookkeeping that the guest cannot see (heap free lists, open files, DirectDraw/Direct3D/DirectSound/
 * DirectInput objects, timers, clocks).  Host-only caches (texture caches, GPU textures, queued audio) are rebuilt after a load.
 *
 * Requests come from any thread (rbo_state_request) and are carried out by the emulator thread at the top of cpu_run, i.e. between two guest
 * instructions with no nested guest call on the host stack.  Files are written to "<path>.tmp" and renamed, and the size is checked before a
 * load touches anything, so an interrupted save can never destroy the old slot or half-restore a game. */
#include "hle.h"
#include "state.h"
#include "rbo_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <zlib.h>

#define ST_MAGIC   0x31535452u      /* "RTS1" */
#define ST_VERSION 4
#define PAGE       4096u
#define MEM_END_MARK 0xFFFFFFFFu

typedef struct {
    uint32_t magic, version;
    uint32_t ntab; uint64_t tabhash;
    uint32_t dshow, code_lo, code_hi, heap_top;
    int64_t  unix_time; int32_t frames; int32_t pad;
    uint64_t file_size;
} Hdr;

extern uint8_t *gmem;
extern uint32_t g_code_lo, g_code_hi;
extern int g_nest, g_switch, g_retry;
volatile int g_state_req=0;                 /* 1 save, 2 load (set by rbo_state_request) */
static char g_state_path[1100];
static volatile int g_state_res=0;          /* 0 idle, 1 busy, 2 saved, 3 loaded, <0 error */
static char g_state_msg[160]="";

static void result(int code,const char *msg){ snprintf(g_state_msg,sizeof g_state_msg,"%s",msg); __atomic_store_n(&g_state_res,code,__ATOMIC_RELEASE); }

/* guest-memory regions that can ever hold data (see main.c/pe.c/hle.c: TEB+image below 38 MB, bump heap, main stack) */
typedef struct { uint32_t lo, hi; } Region;
static int regions(Region *r){
    uint32_t ht=(hle_heap_top()+PAGE-1)&~(PAGE-1);
    r[0]=(Region){0x00000000u,0x02600000u};   /* TEB, scratch, PE image incl. its 34 MB .data/bss (0x400000-0x2571000) */
    r[1]=(Region){0x04000000u,ht};
    r[2]=(Region){0x1FE00000u,0x20000000u};
    return 3;
}
static int page_is_zero(const uint8_t *p){
    const uint64_t *q=(const uint64_t*)p; uint64_t acc=0;
    for(unsigned i=0;i<PAGE/8;i++) acc|=q[i];
    return acc==0;
}
/* Free heap blocks hold dead data, so a save leaves them out (they come back as zero pages): fr = sorted [lo,hi) pairs from hle_free_ranges.
   RBO_STATE_KEEPFREE=1 stores them anyway. page_get() returns 0 when the page is all zero once the free ranges are blanked out, otherwise it
   returns 1 and leaves the page in `out` (blanked). `*fi` is a cursor: pages must be asked for in ascending address order. */
extern uint32_t hle_free_ranges(uint32_t **out);
typedef struct { uint32_t *r; uint32_t n, i; } FreeMap;
static void freemap_init(FreeMap *m){ m->r=NULL; m->n=0; m->i=0; if(!getenv("RBO_STATE_KEEPFREE")) m->n=hle_free_ranges(&m->r); }
static void freemap_done(FreeMap *m){ free(m->r); m->r=NULL; m->n=0; }
static int page_get(FreeMap *m,uint32_t a,uint8_t *out){
    while(m->i<m->n&&m->r[2*m->i+1]<=a) m->i++;
    if(m->i>=m->n||m->r[2*m->i]>=a+PAGE){ if(page_is_zero(gmem+a)) return 0; memcpy(out,gmem+a,PAGE); return 1; }   /* no free block touches this page */
    memcpy(out,gmem+a,PAGE);
    for(uint32_t j=m->i;j<m->n&&m->r[2*j]<a+PAGE;j++){
        uint32_t lo=m->r[2*j]>a?m->r[2*j]:a, hi=m->r[2*j+1]<a+PAGE?m->r[2*j+1]:a+PAGE;
        if(hi>lo) memset(out+(lo-a),0,hi-lo);
    }
    return !page_is_zero(out);
}
/* Guest memory is stored as chunks of up to CHUNK_PAGES pages: [u32 raw_len][u32 comp_len][deflate data] ... [u32 0].
   A raw chunk is a list of [u32 page index][4096 bytes]. Compression (zlib level 1) brings a 320 MB stage down to about a third. */
#define CHUNK_PAGES 256
#define CHUNK_RAW   (CHUNK_PAGES*(PAGE+4))
static int flush_chunk(StateIO *io,uint8_t *raw,uint32_t rawlen,uint8_t *comp){
    if(rawlen==0) return 0;
    uLongf cl=compressBound(rawlen);
    if(compress2(comp,&cl,raw,rawlen,1)!=Z_OK) return -1;
    uint32_t a=rawlen,b=(uint32_t)cl; STX(io,a); STX(io,b); st_raw(io,comp,cl);
    return io->err?-1:0;
}
static void mem_state(StateIO *io){
    st_tag(io,"MEM0");
    uint8_t *raw=malloc(CHUNK_RAW), *comp=malloc(compressBound(CHUNK_RAW));
    if(!raw||!comp){ free(raw); free(comp); io->err=ST_ERR_IO; return; }
    if(!io->load){
        Region rg[3]; int n=regions(rg); uint32_t pages=0, rawlen=0; FreeMap fm; freemap_init(&fm);
        for(int k=0;k<n&&!io->err;k++) for(uint32_t a=rg[k].lo;a<rg[k].hi&&!io->err;a+=PAGE){
            uint32_t pi=a/PAGE; if(!page_get(&fm,a,raw+rawlen+4)) continue;
            memcpy(raw+rawlen,&pi,4); rawlen+=PAGE+4; pages++;
            if(rawlen==CHUNK_RAW){ if(flush_chunk(io,raw,rawlen,comp)) io->err=ST_ERR_IO; rawlen=0; }
        }
        if(!io->err&&flush_chunk(io,raw,rawlen,comp)) io->err=ST_ERR_IO;
        uint32_t end=0; STX(io,end); freemap_done(&fm);
        fprintf(stderr,"[state] memory: %u pages (%.1f MB) written\n",pages,pages*(double)PAGE/1048576.0);
    } else {
        /* fresh zero mapping at the same address: releases the old pages without touching all 512 MB */
        void *p=mmap(gmem,MEM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
        if(p==MAP_FAILED||p!=(void*)gmem){ io->err=ST_ERR_IO; free(raw); free(comp); return; }
        for(;;){
            uint32_t rl,cl; STX(io,rl); if(io->err||rl==0) break;
            STX(io,cl); if(io->err||rl>CHUNK_RAW||cl>compressBound(CHUNK_RAW)){ io->err=ST_ERR_IO; break; }
            st_raw(io,comp,cl); if(io->err) break;
            uLongf dl=CHUNK_RAW; if(uncompress(raw,&dl,comp,cl)!=Z_OK||dl!=rl||rl%(PAGE+4)){ io->err=ST_ERR_IO; break; }
            for(uint32_t o=0;o<rl;o+=PAGE+4){
                uint32_t pi; memcpy(&pi,raw+o,4);
                if((uint64_t)pi*PAGE+PAGE>(uint64_t)MEM_SIZE){ io->err=ST_ERR_IO; break; }
                memcpy(gmem+(size_t)pi*PAGE,raw+o+4,PAGE);
            }
            if(io->err) break;
        }
    }
    free(raw); free(comp);
}
static void all_modules(StateIO *io){
    cpu_state(io); sched_state(io); hle_state(io); win_state(io); dx_state(io); d3d_state(io); dshow_state(io); hooks_state(io);
    mem_state(io);
    st_tag(io,"END0");
}

uint64_t state_mem_hash(void); int state_stray_pages(void);
static int do_save(const char *path,char *why,size_t wn){
    if(g_nest!=0){ snprintf(why,wn,"지금은 저장할 수 없어요"); return -1; }
    if(dshow_movie_active()){ snprintf(why,wn,"영상 재생 중에는 저장할 수 없어요"); return -2; }
    d3d_state_flush();
    if(getenv("RBO_STATEDBG")) fprintf(stderr,"[state] SAVE frame=%d eip=%08x icount=%llu memhash=%016llx stray=%d\n",g_frames,cpu.eip,(unsigned long long)cpu.icount,(unsigned long long)state_mem_hash(),state_stray_pages());
    if(getenv("RBO_STATEDBG")){ extern uint64_t hle_free_bytes(void); extern uint32_t hle_free_blocks(void); fprintf(stderr,"[state] heap_top=%.1f MB, free-list blocks=%u (%.1f MB)\n",hle_heap_top()/1048576.0,hle_free_blocks(),hle_free_bytes()/1048576.0); }
    char tmp[1200]; snprintf(tmp,sizeof tmp,"%s.tmp",path);
    FILE *f=fopen(tmp,"wb"); if(!f){ snprintf(why,wn,"파일을 만들 수 없어요"); return -3; }
    static char iobuf[1<<20]; setvbuf(f,iobuf,_IOFBF,sizeof iobuf);
    Hdr h; memset(&h,0,sizeof h);
    h.magic=ST_MAGIC; h.version=ST_VERSION; h.ntab=hle_tab_count(); h.tabhash=hle_tab_hash(); h.dshow=dshow_graph_active();
    h.code_lo=g_code_lo; h.code_hi=g_code_hi; h.heap_top=hle_heap_top(); h.unix_time=(int64_t)time(NULL); h.frames=g_frames;
    fwrite(&h,sizeof h,1,f);
    StateIO io={f,0,0}; all_modules(&io);
    h.file_size=(uint64_t)ftell(f);
    fseek(f,0,SEEK_SET); fwrite(&h,sizeof h,1,f);
    int bad=io.err||fflush(f)!=0||ferror(f); fclose(f);
    if(bad){ unlink(tmp); snprintf(why,wn,"저장 중 오류 (저장 공간이 부족할 수 있어요)"); return -4; }
    if(rename(tmp,path)!=0){ unlink(tmp); snprintf(why,wn,"저장 파일을 바꿀 수 없어요"); return -5; }
    snprintf(why,wn,"저장했어요"); return 0;
}
static int read_hdr(const char *path,Hdr *h){
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    int ok=fread(h,sizeof *h,1,f)==1; fclose(f);
    return ok&&h->magic==ST_MAGIC?0:-2;
}
static int do_load(const char *path,char *why,size_t wn){
    if(g_nest!=0){ snprintf(why,wn,"지금은 불러올 수 없어요"); return -1; }
    Hdr h; int r=read_hdr(path,&h);
    if(r==-1){ snprintf(why,wn,"저장된 게임이 없어요"); return -2; }
    if(r!=0||h.version!=ST_VERSION){ snprintf(why,wn,"저장 파일 형식이 맞지 않아요"); return -3; }
    struct stat st; if(stat(path,&st)!=0||(uint64_t)st.st_size!=h.file_size){ snprintf(why,wn,"저장 파일이 손상되었어요"); return -4; }
    if(h.code_lo!=g_code_lo||h.code_hi!=g_code_hi){ snprintf(why,wn,"다른 실행 파일의 저장이에요"); return -5; }
    /* the DirectShow COM tables are created lazily by the game; a fresh process has to create them first so that the thunk indexes line up */
    if(h.dshow&&!dshow_graph_active()) dshow_make_graph();
    if(h.ntab!=hle_tab_count()||h.tabhash!=hle_tab_hash()){ snprintf(why,wn,"이 버전과 호환되지 않는 저장이에요"); return -6; }
    FILE *f=fopen(path,"rb"); if(!f){ snprintf(why,wn,"파일을 열 수 없어요"); return -7; }
    static char iobuf[1<<20]; setvbuf(f,iobuf,_IOFBF,sizeof iobuf);
    fseek(f,sizeof h,SEEK_SET);
    StateIO io={f,1,0}; all_modules(&io); fclose(f);
    if(io.err){ fprintf(stderr,"[state] load failed (err %d) - state is inconsistent\n",io.err); snprintf(why,wn,"불러오기 실패 (게임을 다시 시작해 주세요)"); return -8; }
    g_retry=0; g_switch=0;
    { extern void rbo_cheat_reset(void); rbo_cheat_reset(); }   /* frozen cheat values belong to the old timeline */
    { extern void rbo_hooks_resync(void); rbo_hooks_resync(); }   /* a state saved before a hook existed holds the original code: patch it again */
    if(getenv("RBO_STATEDBG")) fprintf(stderr,"[state] LOAD frame=%d eip=%08x icount=%llu memhash=%016llx\n",g_frames,cpu.eip,(unsigned long long)cpu.icount,(unsigned long long)state_mem_hash());
    snprintf(why,wn,"불러왔어요"); return 0;
}

/* called from cpu_run on the emulator thread */
void state_service(void){
    int req=__atomic_exchange_n(&g_state_req,0,__ATOMIC_ACQ_REL); if(!req) return;
    char why[160]; int r;
    if(req==1){ r=do_save(g_state_path,why,sizeof why); result(r==0?2:r,why); }
    else      { r=do_load(g_state_path,why,sizeof why); result(r==0?3:r,why); }
}

/* test aid: hash of all guest memory that a state covers + CPU registers (compare a continued run with a loaded one) */
uint64_t state_mem_hash(void){
    uint64_t h=1469598103934665603ull; Region rg[3]; int n=regions(rg); FreeMap fm; freemap_init(&fm); static uint8_t pg[PAGE];
    for(int k=0;k<n;k++) for(uint32_t a=rg[k].lo;a<rg[k].hi;a+=PAGE){
        if(!page_get(&fm,a,pg)) continue;
        h^=a; h*=1099511628211ull;
        const uint64_t *q=(const uint64_t*)pg; for(unsigned i=0;i<PAGE/8;i++){ h^=q[i]; h*=1099511628211ull; }
    }
    freemap_done(&fm);
    for(int i=0;i<8;i++){ h^=cpu.r[i]; h*=1099511628211ull; } h^=cpu.eip; h*=1099511628211ull;
    return h;
}
/* test aid: count non-zero pages of the whole address space that lie OUTSIDE the saved regions (must be 0) */
int state_stray_pages(void){
    Region rg[3]; int n=regions(rg); int stray=0;
    for(uint32_t a=0;a<MEM_SIZE;a+=PAGE){
        int in=0; for(int k=0;k<n;k++) if(a>=rg[k].lo&&a<rg[k].hi) in=1;
        if(!in&&!page_is_zero(gmem+a)){ if(stray<5) fprintf(stderr,"[state] stray page %08x\n",a); stray++; }
    }
    return stray;
}
int rbo_state_request(int op,const char *path){
    if(op!=1&&op!=2) return -1;
    if(__atomic_load_n(&g_state_res,__ATOMIC_ACQUIRE)==1||g_state_req) return -2;     /* busy */
    snprintf(g_state_path,sizeof g_state_path,"%s",path);
    result(1,"");
    __atomic_store_n(&g_state_req,op,__ATOMIC_RELEASE);
    return 0;
}
int rbo_state_result(void){ return __atomic_load_n(&g_state_res,__ATOMIC_ACQUIRE); }
const char *rbo_state_message(void){ return g_state_msg; }
int rbo_state_info(const char *path,int64_t *unix_time,int *frames){
    Hdr h; if(read_hdr(path,&h)!=0) return -1;
    if(unix_time) *unix_time=h.unix_time; if(frames) *frames=h.frames; return 0;
}
