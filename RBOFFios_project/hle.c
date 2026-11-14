#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>

int g_trace=0;
int g_retry=0; extern int g_blocked;
int sched_nthreads(void); void sched_set_waiting(int w); uint64_t *sched_deadline_ptr(int *waiting); uint32_t sched_create(uint32_t start,uint32_t param); int sched_thread_status(uint32_t handle,uint32_t *code); void sched_set_exit_code(uint32_t code); int sched_current(void);
int g_cur_idx=0;
const char *g_root=".";
int g_max_frames=0, g_frames=0;

/* ---------------- profiler ---------------- */
static uint64_t now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }
#define PROF_MAX 4096
static uint64_t pf_ns[PROF_MAX], pf_calls[PROF_MAX];       /* per HLE function: own time (ns) and call count */
static uint64_t g_prof_hle_ns=0;                            /* total own time of all HLE functions */
static uint64_t g_prof_guest_ns=0;                          /* time in nested guest code (guest_call), sleeps excluded */
static uint64_t g_prof_sleep_ns[3]={0,0,0};                 /* sleeping, by source: Sleep / idle tick / vblank wait */
static inline uint64_t prof_sleep_total(void){ return g_prof_sleep_ns[0]+g_prof_sleep_ns[1]+g_prof_sleep_ns[2]; }

/* ---------------- registry ---------------- */
#define MAX_HLE 4096
static HleEntry tab[MAX_HLE];
static int ntab=0;
int hle_register(const char *dll,const char *name,int nargs,hle_fn fn){
    tab[ntab].dll=strdup(dll); tab[ntab].name=strdup(name); tab[ntab].nargs=nargs; tab[ntab].fn=fn; return ntab++;
}
int hle_lookup(const char *dll,const char *name){
    for(int i=0;i<ntab;i++) if(!strcasecmp(tab[i].dll,dll)&&!strcmp(tab[i].name,name)&&tab[i].fn) return i;
    return -1;
}
const char* hle_name(int idx){ return idx>=0&&idx<ntab?tab[idx].name:"?"; }
uint32_t hle_thunk(int idx){ return HLE_BASE+(uint32_t)idx*4; }

void hle_dispatch_impl(uint32_t idx){
    if(idx>=(uint32_t)ntab){ fprintf(stderr,"[BAD HLE idx %u] eip=%08x\n",idx,cpu.eip); abort(); }
    HleEntry *e=&tab[idx];
    if(!e->fn||e->nargs==-1){
        fprintf(stderr,"[UNIMPLEMENTED] %s!%s called (ret=%08x, esp=%08x)\n",e->dll,e->name,rd32(cpu.r[ESP]),cpu.r[ESP]);
        for(int i=0;i<6;i++) fprintf(stderr,"   arg%d=%08x\n",i,A(i));
        fflush(stderr); abort();
    }
    if(g_trace>1) fprintf(stderr,"[hle] %s!%s(%08x,%08x,%08x,%08x)\n",e->dll,e->name,A(0),A(1),A(2),A(3));
    uint32_t ra=rd32(cpu.r[ESP]);
    uint32_t esp0=cpu.r[ESP];
    g_cur_idx=(int)idx; g_retry=0;
    uint64_t pt0=now_ns(), pg0=g_prof_guest_ns, ps0=prof_sleep_total();
    e->fn();
    { uint64_t dt=now_ns()-pt0, inner=(g_prof_guest_ns-pg0)+(prof_sleep_total()-ps0), own=dt>inner?dt-inner:0;
      if(idx<PROF_MAX){ pf_ns[idx]+=own; pf_calls[idx]++; } g_prof_hle_ns+=own; }
    if(cpu.halted||cpu.eip==THREAD_EXIT_MAGIC) return;
    if(e->nargs==-2) return;          /* raw hook (hooks.c): the function set ESP/EIP itself */
    if(g_retry){
        if(g_nest>0||sched_nthreads()==1){ /* cannot block here: report timeout instead of spinning */
            if(sched_nthreads()==1){ /* single thread: let virtual time pass so deadlines expire */
                static int spins=0; if(++spins<200000){ extern void hle_idle_tick(void); hle_idle_tick(); return; }
                spins=0; }
            g_retry=0; sched_set_waiting(0); RET(258);
        } else { g_blocked=1; g_switch=1; return; }
    }
    /* stdcall: pop return + args (nargs>=0). cdecl handled by fn setting nargs=0 */
    cpu.r[ESP]=esp0+4+4*e->nargs;
    cpu.eip=ra;
}

/* ---------------- guest heap ---------------- */
#define HEAP_START 0x04000000u
#define HEAP_END   0x1EF00000u
static uint32_t heap_top=HEAP_START;
typedef struct FreeNode { uint32_t addr; struct FreeNode *next; } FreeNode;
#define NBUCKET 4096
static FreeNode *freelist[NBUCKET];
static uint32_t round_size(uint32_t s){ if(s<16)s=16; if(s<=2048) return (s+15)&~15u; return (s+4095)&~4095u; }
uint32_t ga_alloc(uint32_t size,int zero){
    uint32_t rs=round_size(size);
    unsigned b=(rs/16)%NBUCKET;
    for(FreeNode **pp=&freelist[b];*pp;pp=&(*pp)->next){
        FreeNode *n=*pp; uint32_t hs=rd32(n->addr-16);
        if(hs==rs){ *pp=n->next; uint32_t a=n->addr; free(n); if(zero) memset(gmem+a,0,rs); return a; }
    }
    uint32_t a=heap_top+16;
    if(a+rs>=HEAP_END){ fprintf(stderr,"[OOM] guest heap exhausted (req %u)\n",size); abort(); }
    heap_top=a+rs;
    wr32(a-16,rs); wr32(a-12,0xA110CA7E);
    return a; /* fresh memory is zero */
}
uint32_t ga_size(uint32_t p){ return rd32(p-16); }
void ga_free(uint32_t p){
    if(!p) return;
    if(rd32(p-12)!=0xA110CA7E){ fprintf(stderr,"[WARN] free of non-heap ptr %08x\n",p); return; }
    uint32_t rs=rd32(p-16); unsigned b=(rs/16)%NBUCKET;
    FreeNode *n=malloc(sizeof *n); n->addr=p; n->next=freelist[b]; freelist[b]=n;
}
static uint32_t scratch=0x00200000u;
uint32_t ga_str(const char *s){ uint32_t a=ga_alloc(strlen(s)+1,0); strcpy((char*)gmem+a,s); return a; }

/* ---------------- guest nested calls ---------------- */
uint32_t guest_call(uint32_t fn,int n,const uint32_t *args){
    uint32_t saved_esp=cpu.r[ESP], saved_eip=cpu.eip;
    for(int i=n-1;i>=0;i--){ cpu.r[ESP]-=4; wr32(cpu.r[ESP],args[i]); }
    cpu.r[ESP]-=4; wr32(cpu.r[ESP],EXIT_MAGIC);
    cpu.eip=fn;
    uint64_t gt0=now_ns(), gs0=prof_sleep_total();
    g_nest++; cpu_run(EXIT_MAGIC); g_nest--;
    { uint64_t el=now_ns()-gt0, sl=prof_sleep_total()-gs0; g_prof_guest_ns+= el>sl?el-sl:0; }
    uint32_t r=cpu.r[EAX];
    cpu.r[ESP]=saved_esp; cpu.eip=saved_eip;
    return r;
}

/* ---------------- time ---------------- */
static uint64_t now_us(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000u+ts.tv_nsec/1000; }
static uint64_t t0_us;
uint64_t g_stat_idle_us=0;   /* wall time spent sleeping instead of interpreting (for the on-screen stats) */
void hle_sleep_us(uint64_t us,int src){ uint64_t a=now_ns(); usleep((useconds_t)us); uint64_t d=now_ns()-a; g_stat_idle_us+=d/1000; g_prof_sleep_ns[src<0||src>2?1:src]+=d; }
int g_fast=0; static uint64_t vbase_ms=10000; static int vk=0;
/* fast mode: guest time never runs slower than the frame clock (16.667 ms/frame); intra-frame timeGetTime ticks are
   capped at 16 ms per frame and idle waits advance the base directly (so they are not counted twice). */
static uint64_t vtime_ms(void){ return vbase_ms+(vk>16?16:vk); }
void hle_vsync_advance(void){
    if(!g_fast) return;
    uint64_t ideal=10000+(uint64_t)(g_frames*16.6667), cur=vtime_ms();
    vbase_ms = cur>ideal?cur:ideal; vk=0;
    if(g_trace&&g_frames%300==0) fprintf(stderr,"[clock] frames=%d vtime=%llu ms (ideal %llu ms)\n",g_frames,(unsigned long long)vbase_ms,(unsigned long long)ideal);
}
uint64_t hle_now_us(void){ if(g_fast) return vtime_ms()*1000ull; return now_us()-t0_us+10000000ull; }
static uint32_t vnow(void){ if(g_fast){ vk++; return (uint32_t)vtime_ms(); } return (uint32_t)((now_us()-t0_us)/1000)+10000; }

/* ---------------- files ---------------- */
#define MAXH 256
static struct { int fd; int used; char path[96]; } hs[MAXH];
static uint32_t last_error=0;
static int resolve_path_slow(const char *gpath,char *out,size_t outsz){
    char tmp[1024]; size_t n=0;
    for(const char*p=gpath;*p&&n<sizeof tmp-1;p++) tmp[n++]=(*p=='\\')?'/':*p;
    tmp[n]=0;
    char *s=tmp;
    if(s[0]&&s[1]==':') s+=2;
    if(s[0]=='.'&&s[1]=='/') s+=2;
    while(s[0]=='/') s++;
    snprintf(out,outsz,"%s",g_root);
    char *save,*tok;
    for(tok=strtok_r(s,"/",&save);tok;tok=strtok_r(NULL,"/",&save)){
        if(!strcmp(tok,".")) continue;
        DIR *d=opendir(out); if(!d) return 0;
        struct dirent *de; int found=0;
        while((de=readdir(d))){ if(!strcasecmp(de->d_name,tok)){ size_t l=strlen(out); snprintf(out+l,outsz-l,"/%s",de->d_name); found=1; break; } }
        closedir(d);
        if(!found){
            size_t l=strlen(out); snprintf(out+l,outsz-l,"/%s",tok);
            while((tok=strtok_r(NULL,"/",&save))){ l=strlen(out); snprintf(out+l,outsz-l,"/%s",tok); }
            return 0;
        }
    }
    return 1;
}
/* Cache of successful lookups.  The slow path opendir()s every directory of the path and scans it with strcasecmp, which is
   expensive in big asset directories and runs for every CreateFileA/PathFileExistsA (hundreds per second when a message loads
   its portrait/voice).  Only existing files are cached (a missing file may be created later); a hit is re-checked with access(). */
#define RP_SLOTS 2048
static struct { uint32_t h; char *key; char *host; } rp_cache[RP_SLOTS];
static uint32_t rp_hash(const char *k){ uint32_t h=2166136261u; for(;*k;k++){ h^=(uint8_t)*k; h*=16777619u; } return h|1u; }
int resolve_path(const char *gpath,char *out,size_t outsz){
    char key[1024]; size_t n=0;
    for(const char *p=gpath;*p&&n<sizeof key-1;p++){ char c=(*p=='\\')?'/':*p; if(c>='A'&&c<='Z') c+=32; key[n++]=c; }
    key[n]=0;
    uint32_t h=rp_hash(key), i=h%RP_SLOTS;
    for(int probe=0;probe<8;probe++,i=(i+1)%RP_SLOTS){
        if(rp_cache[i].h==h && rp_cache[i].key && !strcmp(rp_cache[i].key,key)){
            if(strlen(rp_cache[i].host)<outsz && access(rp_cache[i].host,F_OK)==0){ snprintf(out,outsz,"%s",rp_cache[i].host); return 1; }
            free(rp_cache[i].key); free(rp_cache[i].host); rp_cache[i].key=rp_cache[i].host=NULL; rp_cache[i].h=0; break;
        }
    }
    int ex=resolve_path_slow(gpath,out,outsz);
    if(ex){
        i=h%RP_SLOTS; int slot=-1;
        for(int probe=0;probe<8;probe++,i=(i+1)%RP_SLOTS) if(!rp_cache[i].key){ slot=(int)i; break; }
        if(slot<0) slot=(int)(h%RP_SLOTS);                       /* all 8 probes taken: replace the first */
        free(rp_cache[slot].key); free(rp_cache[slot].host);
        rp_cache[slot].h=h; rp_cache[slot].key=strdup(key); rp_cache[slot].host=strdup(out);
    }
    return ex;
}
static uint32_t newhandle(int fd){ for(int i=1;i<MAXH;i++) if(!hs[i].used){ hs[i].used=1; hs[i].fd=fd; hs[i].path[0]=0; return 0x1000+i*4; } return 0xFFFFFFFF; }
static int hfd(uint32_t h){ int i=(h-0x1000)/4; if(h<0x1000||i<=0||i>=MAXH||!hs[i].used) return -1; return hs[i].fd; }

static void h_CreateFileA(void){
    const char *p=(const char*)gmem+A(0); uint32_t acc=A(1), disp=A(4);
    char host[1200]; int ex=resolve_path(p,host,sizeof host);
    int flags; int w=(acc&0x40000000u)!=0;
    switch(disp){ case 1: flags=O_RDWR|O_CREAT|O_EXCL; break; case 2: flags=O_RDWR|O_CREAT|O_TRUNC; break; case 3: flags=w?O_RDWR:O_RDONLY; break; case 4: flags=O_RDWR|O_CREAT; break; default: flags=O_RDWR|O_TRUNC; }
    if(!w&&disp==3) flags=O_RDONLY;
    if(g_trace) fprintf(stderr,"[CreateFileA] %s -> %s (%s)\n",p,host,ex?"exists":"missing");
    int fd=open(host,flags,0644);
    if(fd<0){ last_error= ex?5:2; RET(0xFFFFFFFF); return; }
    last_error=0; { uint32_t hh=newhandle(fd); int ii=(hh-0x1000)/4; if(hh!=0xFFFFFFFF) snprintf(hs[ii].path,sizeof hs[ii].path,"%s",p); RET(hh); }
}
static void h_ReadFile(void){
    int fd=hfd(A(0)); uint32_t buf=A(1), n=A(2), pr=A(3);
    if(fd<0){ RET(0); return; }
    if(buf+n>=MEM_SIZE) gfault("ReadFile buf",buf);
    ssize_t r=read(fd,gmem+buf,n);
    if(r<0){ RET(0); return; }
    if(g_trace>0 && getenv("RBO_READTRACE")){ int ii=(A(0)-0x1000)/4; static int c=0; if(c++<4000) fprintf(stderr,"[read] frame %d %s %u bytes\n",g_frames,hs[ii].path,n); }
    if(pr) wr32(pr,(uint32_t)r);
    RET(1);
}
static void h_WriteFile(void){
    int fd=hfd(A(0)); uint32_t buf=A(1), n=A(2), pw=A(3);
    if(A(0)<=3){ fwrite(gmem+buf,1,n,stderr); if(pw)wr32(pw,n); RET(1); return; }
    if(fd<0){ RET(0); return; }
    ssize_t r=write(fd,gmem+buf,n); if(pw) wr32(pw,r<0?0:(uint32_t)r); RET(r>=0);
}
static void h_SetFilePointer(void){
    int fd=hfd(A(0)); if(fd<0){ RET(0xFFFFFFFF); return; }
    int64_t off=(int32_t)A(1); uint32_t ph=A(2); if(ph) off=((int64_t)(int32_t)rd32(ph)<<32)|(uint32_t)A(1);
    int wh=A(3)==0?SEEK_SET:A(3)==1?SEEK_CUR:SEEK_END;
    off_t r=lseek(fd,off,wh); if(r<0){ RET(0xFFFFFFFF); return; }
    if(ph) wr32(ph,(uint32_t)((uint64_t)r>>32));
    RET((uint32_t)r);
}
static void h_GetFileSize(void){
    int fd=hfd(A(0)); if(fd<0){RET(0xFFFFFFFF);return;}
    struct stat st; fstat(fd,&st); if(A(1)) wr32(A(1),(uint32_t)((uint64_t)st.st_size>>32)); RET((uint32_t)st.st_size);
}
static void h_CloseHandle(void){ int i=(A(0)-0x1000)/4; if(A(0)>=0x1000&&i>0&&i<MAXH&&hs[i].used){ close(hs[i].fd); hs[i].used=0; } RET(1); }
static void h_DeleteFileA(void){ char host[1200]; if(resolve_path((char*)gmem+A(0),host,sizeof host)) RET(unlink(host)==0); else RET(0); }
static void h_SetFileAttributesA(void){ RET(1); }

/* ---------------- process / misc ---------------- */
static uint32_t cmdline_ptr, env_ptr;
static void h_GetCommandLineA(void){ RET(cmdline_ptr); }
static void h_GetStartupInfoA(void){ uint32_t p=A(0); memset(gmem+p,0,68); wr32(p,68); }
static void h_GetVersion(void){ RET(0x0A280105u); }
static void h_GetACP(void){ RET(949); }
static void h_GetOEMCP(void){ RET(949); }
static void h_GetCPInfo(void){ uint32_t p=A(1); memset(gmem+p,0,20); wr32(p,1); wr8(p+4,'?'); RET(1); }
static void h_GetEnvironmentStrings(void){ RET(env_ptr); }
static void h_GetEnvironmentStringsW(void){ RET(env_ptr); }
static void h_FreeEnvironmentStrings(void){ RET(1); }
static void h_SetHandleCount(void){ RET(A(0)); }
static void h_GetStdHandle(void){ RET(A(0)==(uint32_t)-10?1:A(0)==(uint32_t)-11?2:3); }
static void h_GetFileType(void){ RET(2); }
static void h_SetStdHandle(void){ RET(1); }
static void h_GetModuleHandleA(void){ RET(A(0)?0x70000000u:0x400000u); }
static void h_GetModuleFileNameA(void){ const char *s="C:\\RBO\\rbo.exe"; uint32_t b=A(1); strcpy((char*)gmem+b,s); RET(strlen(s)); }
static void h_GetProcAddress(void){ if(g_trace)fprintf(stderr,"[GetProcAddress] %s\n",(A(1)>0xFFFF)?(char*)gmem+A(1):"(ord)"); RET(0); }
static void h_LoadLibraryA(void){ if(g_trace)fprintf(stderr,"[LoadLibraryA] %s\n",(char*)gmem+A(0)); RET(0); }
static void h_ExitProcess(void){ fprintf(stderr,"[ExitProcess] code=%u frames=%d icount=%llu\n",A(0),g_frames,(unsigned long long)cpu.icount); cpu.halted=1; }
static void h_TerminateProcess(void){ h_ExitProcess(); }
static void h_GetCurrentProcess(void){ RET(0xFFFFFFFF); }
static void h_GetLastError(void){ RET(last_error); }
static void h_UnhandledExceptionFilter(void){ RET(1); }
static void h_SetUnhandledExceptionFilter(void){ RET(0); }
static void h_RtlUnwind(void){ fprintf(stderr,"[RtlUnwind] unsupported\n"); abort(); }
static void h_RaiseException(void){ fprintf(stderr,"[RaiseException] code=%08x\n",A(0)); abort(); }
static void h_Sleep(void){
    if(sched_nthreads()>1){ /* multi-threaded: yield until the deadline passes */
        int waiting; uint64_t *dl=sched_deadline_ptr(&waiting); uint64_t now=hle_now_us();
        if(!waiting){ sched_set_waiting(1); *dl=now+(uint64_t)A(0)*1000ull; g_retry=1; return; }
        if(now<*dl){ g_retry=1; return; }
        sched_set_waiting(0); RET(0); return; }
    if(g_fast) vk+=(int)A(0); else hle_sleep_us(A(0)*1000u,0); RET(0); }
void hle_idle_tick(void){ if(g_fast) vbase_ms+=1; else hle_sleep_us(250,1); }   /* 250us (was 1000): the guest clock is real time, a shorter nap lets the frame limiter wake closer to its target */
static void h_GetTickCount(void){ RET(vnow()); }
static void h_timeGetTime(void){ RET(vnow()); }
static void h_QueryPerformanceCounter(void){ uint64_t t=now_us()-t0_us; wr32(A(0),(uint32_t)t); wr32(A(0)+4,(uint32_t)(t>>32)); RET(1); }
static void h_QueryPerformanceFrequency(void){ wr32(A(0),1000000); wr32(A(0)+4,0); RET(1); }
static void fill_systime(uint32_t p,int local){
    time_t t=time(NULL); struct tm tm; if(local)localtime_r(&t,&tm); else gmtime_r(&t,&tm);
    wr16(p,tm.tm_year+1900); wr16(p+2,tm.tm_mon+1); wr16(p+4,tm.tm_wday); wr16(p+6,tm.tm_mday);
    wr16(p+8,tm.tm_hour); wr16(p+10,tm.tm_min); wr16(p+12,tm.tm_sec); wr16(p+14,0);
}
static void h_GetSystemTime(void){ fill_systime(A(0),0); }
static void h_GetLocalTime(void){ fill_systime(A(0),1); }
static void h_GetSystemTimeAsFileTime(void){ uint64_t t=(uint64_t)time(NULL)*10000000ull+116444736000000000ull; wr32(A(0),(uint32_t)t); wr32(A(0)+4,(uint32_t)(t>>32)); }
static void h_GetTimeZoneInformation(void){ memset(gmem+A(0),0,172); RET(0); }
static void h_GetWindowsDirectoryA(void){ strcpy((char*)gmem+A(0),"C:\\WINDOWS"); RET(10); }

static void h_HeapCreate(void){ RET(0x00500000u); }
static void h_HeapDestroy(void){ RET(1); }
static void h_HeapAlloc(void){ RET(ga_alloc(A(2),(A(1)&8)!=0)); }
static void h_HeapFree(void){ ga_free(A(2)); RET(1); }
static void h_HeapSize(void){ RET(ga_size(A(2))); }
static void h_HeapReAlloc(void){
    uint32_t p=A(2), n=A(3);
    if(!p){ RET(ga_alloc(n,(A(1)&8)!=0)); return; }
    uint32_t old=ga_size(p); if(n<=old){ RET(p); return; }
    uint32_t q=ga_alloc(n,(A(1)&8)!=0); memcpy(gmem+q,gmem+p,old); ga_free(p); RET(q);
}
static void h_GlobalAlloc(void){ RET(ga_alloc(A(1),(A(0)&0x40)!=0)); }
static void h_GlobalFree(void){ ga_free(A(0)); RET(0); }
static void h_VirtualAlloc(void){
    uint32_t addr=A(0), sz=A(1), type=A(2);
    if(addr && !(type&0x2000)) { RET(addr); return; } /* commit inside reserved */
    uint32_t a=ga_alloc((sz+4095)&~4095u,1); RET((a+4095)&~4095u);
}
static void h_VirtualFree(void){ RET(1); }
static void h_IsBadReadPtr(void){ RET(A(0)==0); }
static void h_IsBadWritePtr(void){ RET(A(0)==0); }
static void h_IsBadCodePtr(void){ RET(A(0)==0); }
static void h_lstrcpyA(void){ strcpy((char*)gmem+A(0),(char*)gmem+A(1)); RET(A(0)); }
static void h_WideCharToMultiByte(void){
    /* (cp, flags, wstr, wlen, mb, mblen, def, used) - ASCII only */
    uint32_t ws=A(2); int wl=(int)A(3); uint32_t mb=A(4); int ml=(int)A(5);
    if(wl<0){ wl=0; while(rd16(ws+2*wl)) wl++; wl++; }
    if(ml==0){ RET(wl); return; }
    int n=wl<ml?wl:ml; for(int i=0;i<n;i++) wr8(mb+i,(uint8_t)rd16(ws+2*i)); RET(n);
}
static void h_MultiByteToWideChar(void){
    uint32_t mb=A(2); int ml=(int)A(3); uint32_t ws=A(4); int wl=(int)A(5);
    if(ml<0){ ml=0; while(rd8(mb+ml)) ml++; ml++; }
    if(wl==0){ RET(ml); return; }
    int n=ml<wl?ml:wl; for(int i=0;i<n;i++) wr16(ws+2*i,rd8(mb+i)); RET(n);
}
static void h_LCMapStringA(void){ int n=(int)A(3); if(n<0){ n=strlen((char*)gmem+A(2))+1; } if(A(5)){ int m=(int)A(5); if(m<n)n=m; memcpy(gmem+A(4),gmem+A(2),n);} RET(n); }
static void h_LCMapStringW(void){ int n=(int)A(3); if(n<0){ n=0; while(rd16(A(2)+2*n)) n++; n++; } if(A(5)){ int m=(int)A(5); if(m<n)n=m; memcpy(gmem+A(4),gmem+A(2),n*2);} RET(n); }
static void h_GetStringTypeA(void){ int n=(int)A(3); if(n<0) n=strlen((char*)gmem+A(2))+1; for(int i=0;i<n;i++){ unsigned c=rd8(A(2)+i); uint16_t t=0; if(isupper(c))t|=1; if(islower(c))t|=2; if(isdigit(c))t|=4; if(isspace(c))t|=8; if(ispunct(c))t|=0x10; if(iscntrl(c))t|=0x20; if(c==' ')t|=0x40; if(isxdigit(c))t|=0x80; if(isalpha(c))t|=0x100; wr16(A(4)+2*i,t);} RET(1); }
static void h_GetStringTypeW(void){ int n=(int)A(2); if(n<0){ n=0; while(rd16(A(1)+2*n)) n++; n++; } for(int i=0;i<n;i++){ unsigned c=rd16(A(1)+2*i); uint16_t t=0; if(c<128){ if(isupper(c))t|=1; if(islower(c))t|=2; if(isdigit(c))t|=4; if(isspace(c))t|=8; if(ispunct(c))t|=0x10; if(iscntrl(c))t|=0x20; if(isalpha(c))t|=0x100; } wr16(A(3)+2*i,t);} RET(1); }
static void h_CompareStringA(void){ int c=strcmp((char*)gmem+A(2),(char*)gmem+A(4)); RET(c<0?1:c>0?3:2); }
static void h_CompareStringW(void){ RET(2); }
static void h_SetEnvironmentVariableA(void){ RET(1); }
static void h_FlushFileBuffers(void){ RET(1); }
static void h_CharUpperA(void){ uint32_t a=A(0); if(a<0x10000){ RET(toupper(a&0xFF)); return; } for(;rd8(a);a++) wr8(a,toupper(rd8(a))); RET(A(0)); }
static void h_wsprintfA(void){
    /* cdecl: wsprintfA(buf, fmt, ...) */
    uint32_t out=A(0); const char *f=(const char*)gmem+A(1); int ai=2; char *o=(char*)gmem+out;
    while(*f){
        if(*f!='%'){ *o++=*f++; continue; }
        f++; char spec[16]; int k=0; spec[k++]='%';
        while(*f&&strchr("-+ #0123456789.l",*f)&&k<14) spec[k++]=*f++;
        char c=*f++; char tmp[512];
        if(c=='s'){ spec[k++]='s'; spec[k]=0; snprintf(tmp,sizeof tmp,spec,(char*)gmem+A(ai++)); }
        else if(c=='c'){ spec[k++]='c'; spec[k]=0; snprintf(tmp,sizeof tmp,spec,(int)A(ai++)); }
        else if(c=='%'){ strcpy(tmp,"%"); }
        else { spec[k++]=c; spec[k]=0; if(strchr(spec,'l')) { char *l=strchr(spec,'l'); memmove(l,l+1,strlen(l)); } snprintf(tmp,sizeof tmp,spec,(unsigned)A(ai++)); }
        strcpy(o,tmp); o+=strlen(tmp);
    }
    *o=0; RET(o-(char*)gmem-out);
}

/* ---------------- threads / sync objects ---------------- */
typedef struct { int type; int signaled, manual; int count, maxcount; } Obj;   /* 1 event, 2 semaphore */
static Obj objs[256]; static int nobjs=0;
static Obj *obj_of(uint32_t h){ if(h<0x3000||h>=0x3000+256*4) return NULL; int i=(h-0x3000)/4; return i<nobjs?&objs[i]:NULL; }
static uint32_t obj_new(int type,int signaled,int manual,int count,int maxc){ Obj *o=&objs[nobjs]; o->type=type; o->signaled=signaled; o->manual=manual; o->count=count; o->maxcount=maxc; return 0x3000+4*(nobjs++); }
static void h_CreateEventA(void){ RET(obj_new(1,A(2)!=0,A(1)!=0,0,0)); }
uint32_t hle_make_event(void){ return obj_new(1,0,1,0,0); }
void hle_set_event(uint32_t h){ Obj *o=obj_of(h); if(o&&o->type==1) o->signaled=1; }
static void h_SetEvent(void){ hle_set_event(A(0)); RET(1); }
static void h_CreateSemaphoreA(void){ RET(obj_new(2,0,0,(int)A(1),(int)A(2))); }
static void h_ReleaseSemaphore(void){ Obj *o=obj_of(A(0)); if(!o){RET(0);return;} if(A(2)) wr32(A(2),o->count); o->count+=(int)A(1); if(o->count>o->maxcount) o->count=o->maxcount; RET(1); }
/* try to satisfy a wait on handle h; 1 if satisfied (and consumed) */
static int wait_try(uint32_t h){
    Obj *o=obj_of(h);
    if(o){ if(o->type==1){ if(o->signaled){ if(!o->manual) o->signaled=0; return 1; } return 0; }
           if(o->type==2){ if(o->count>0){ o->count--; return 1; } return 0; } }
    uint32_t code; int st=sched_thread_status(h,&code); if(st==1) return 1; if(st==0) return 0;
    return 1; /* unknown handle (file etc.): treat as signaled */
}
static int wait_expired(uint32_t timeout){
    int waiting; uint64_t *dl=sched_deadline_ptr(&waiting); uint64_t now=hle_now_us();
    if(!waiting){ sched_set_waiting(1); *dl=(timeout==0xFFFFFFFFu)?~0ull:now+(uint64_t)timeout*1000ull; return 0; }
    return now>=*dl;
}
static void h_WaitForSingleObject(void){
    uint32_t h=A(0), to=A(1);
    if(wait_try(h)){ sched_set_waiting(0); RET(0); return; }
    if(to==0){ sched_set_waiting(0); RET(258); return; }
    if(wait_expired(to)){ if(g_trace) fprintf(stderr,"[wait] timeout h=%08x ret=%08x frame %d\n",h,rd32(cpu.r[ESP]),g_frames); sched_set_waiting(0); RET(258); return; }
    { static int n=0; if(g_trace&&n++<12) fprintf(stderr,"[wait] block h=%08x to=%u ret=%08x frame %d obj=%d\n",h,to,rd32(cpu.r[ESP]),g_frames,obj_of(h)?obj_of(h)->type:-1); }
    g_retry=1;
}
static void h_WaitForMultipleObjects(void){
    uint32_t n=A(0), hp=A(1), all=A(2), to=A(3);
    if(all){ int ok=1; for(uint32_t i=0;i<n;i++){ Obj *o=obj_of(rd32(hp+4*i)); uint32_t c; int st=sched_thread_status(rd32(hp+4*i),&c);
            int sig = o ? (o->type==1?o->signaled:o->count>0) : (st==1||st<0); if(!sig) ok=0; }
        if(ok){ for(uint32_t i=0;i<n;i++) wait_try(rd32(hp+4*i)); sched_set_waiting(0); RET(0); return; } }
    else for(uint32_t i=0;i<n;i++) if(wait_try(rd32(hp+4*i))){ sched_set_waiting(0); RET(i); return; }
    if(to==0){ sched_set_waiting(0); RET(258); return; }
    if(wait_expired(to)){ sched_set_waiting(0); RET(258); return; }
    g_retry=1;
}
static void h_CreateThread(void){
    uint32_t h=sched_create(A(2),A(3)); if(A(5)) wr32(A(5),0x1000+sched_nthreads());
    RET(h);
}
static void h_ExitThread(void){ sched_set_exit_code(A(0)); cpu.eip=THREAD_EXIT_MAGIC; cpu.r[EAX]=A(0); /* handled by cpu_run */ }
static void h_TerminateThread(void){ RET(1); }
static void h_GetExitCodeThread(void){ uint32_t c=259; sched_thread_status(A(0),&c); if(A(1)) wr32(A(1),c); RET(1); }
/* critical sections: RTL_CRITICAL_SECTION owner at +12 (thread id), recursion at +8 */
static void h_InitCS(void){ memset(gmem+A(0),0,24); }
static void h_EnterCS(void){
    uint32_t cs=A(0), me=sched_current()+1, owner=rd32(cs+12);
    if(owner==0||owner==me||sched_nthreads()==1){ wr32(cs+12,me); wr32(cs+8,rd32(cs+8)+1); return; }
    g_retry=1;
}
static void h_LeaveCS(void){ uint32_t cs=A(0); uint32_t r=rd32(cs+8); if(r>0) r--; wr32(cs+8,r); if(r==0) wr32(cs+12,0); }
static void h_CS(void){ }
static void h_UnmapViewOfFile(void){ RET(1); }
static void h_zero(void){ RET(0); }
static void h_one(void){ RET(1); }

#define R(dll,name,n,fn) hle_register(dll,name,n,fn)
void hle_init(void){
    t0_us=now_us(); g_fast=getenv("RBO_FAST")!=NULL;
    hle_dispatch=hle_dispatch_impl;
    cmdline_ptr=ga_str("rbo.exe");
    env_ptr=ga_alloc(16,1);
    const char *K="KERNEL32.dll";
    R(K,"GetCommandLineA",0,h_GetCommandLineA); R(K,"GetStartupInfoA",1,h_GetStartupInfoA);
    R(K,"GetSystemTime",1,h_GetSystemTime); R(K,"GetLocalTime",1,h_GetLocalTime);
    R(K,"GetSystemTimeAsFileTime",1,h_GetSystemTimeAsFileTime); R(K,"GetTimeZoneInformation",1,h_GetTimeZoneInformation);
    R(K,"HeapReAlloc",4,h_HeapReAlloc); R(K,"HeapSize",3,h_HeapSize); R(K,"HeapDestroy",1,h_HeapDestroy);
    R(K,"HeapCreate",3,h_HeapCreate); R(K,"HeapAlloc",3,h_HeapAlloc); R(K,"HeapFree",3,h_HeapFree);
    R(K,"RtlUnwind",4,h_RtlUnwind); R(K,"TerminateProcess",2,h_TerminateProcess); R(K,"ExitProcess",1,h_ExitProcess);
    R(K,"GetVersion",0,h_GetVersion); R(K,"GetACP",0,h_GetACP); R(K,"GetOEMCP",0,h_GetOEMCP); R(K,"GetCPInfo",2,h_GetCPInfo);
    R(K,"UnhandledExceptionFilter",1,h_UnhandledExceptionFilter); R(K,"SetUnhandledExceptionFilter",1,h_SetUnhandledExceptionFilter);
    R(K,"FreeEnvironmentStringsA",1,h_FreeEnvironmentStrings); R(K,"FreeEnvironmentStringsW",1,h_FreeEnvironmentStrings);
    R(K,"GetEnvironmentStrings",0,h_GetEnvironmentStrings); R(K,"GetEnvironmentStringsW",0,h_GetEnvironmentStringsW);
    R(K,"WideCharToMultiByte",8,h_WideCharToMultiByte); R(K,"MultiByteToWideChar",6,h_MultiByteToWideChar);
    R(K,"SetHandleCount",1,h_SetHandleCount); R(K,"GetStdHandle",1,h_GetStdHandle); R(K,"GetFileType",1,h_GetFileType);
    R(K,"VirtualFree",3,h_VirtualFree); R(K,"VirtualAlloc",4,h_VirtualAlloc);
    R(K,"IsBadWritePtr",2,h_IsBadWritePtr); R(K,"IsBadReadPtr",2,h_IsBadReadPtr); R(K,"IsBadCodePtr",1,h_IsBadCodePtr);
    R(K,"GlobalFree",1,h_GlobalFree); R(K,"GlobalAlloc",2,h_GlobalAlloc);
    R(K,"Sleep",1,h_Sleep); R(K,"QueryPerformanceCounter",1,h_QueryPerformanceCounter); R(K,"QueryPerformanceFrequency",1,h_QueryPerformanceFrequency);
    R(K,"GetProcAddress",2,h_GetProcAddress); R(K,"LoadLibraryA",1,h_LoadLibraryA); R(K,"GetTickCount",0,h_GetTickCount);
    R(K,"GetWindowsDirectoryA",2,h_GetWindowsDirectoryA); R(K,"DeleteFileA",1,h_DeleteFileA);
    R(K,"SetUnhandledExceptionFilter",1,h_SetUnhandledExceptionFilter); R(K,"GetModuleHandleA",1,h_GetModuleHandleA);
    R(K,"GetFileSize",2,h_GetFileSize); R(K,"CreateFileA",7,h_CreateFileA); R(K,"CreateEventA",4,h_CreateEventA);
    R(K,"CloseHandle",1,h_CloseHandle); R(K,"UnmapViewOfFile",1,h_UnmapViewOfFile); R(K,"GetExitCodeThread",2,h_GetExitCodeThread);
    R(K,"SetEvent",1,h_SetEvent); R(K,"CreateThread",6,h_CreateThread); R(K,"WaitForMultipleObjects",4,h_WaitForMultipleObjects);
    R(K,"ExitThread",1,h_ExitThread); R(K,"TerminateThread",2,h_TerminateThread);
    R(K,"LeaveCriticalSection",1,h_LeaveCS); R(K,"EnterCriticalSection",1,h_EnterCS); R(K,"DeleteCriticalSection",1,h_CS); R(K,"InitializeCriticalSection",1,h_InitCS);
    R(K,"SetFileAttributesA",2,h_SetFileAttributesA); R(K,"ReadFile",5,h_ReadFile); R(K,"WriteFile",5,h_WriteFile);
    R(K,"SetFilePointer",4,h_SetFilePointer); R(K,"CreateSemaphoreA",4,h_CreateSemaphoreA); R(K,"ReleaseSemaphore",3,h_ReleaseSemaphore);
    R(K,"WaitForSingleObject",2,h_WaitForSingleObject); R(K,"lstrcpyA",2,h_lstrcpyA); R(K,"GetLastError",0,h_GetLastError);
    R(K,"GetCurrentProcess",0,h_GetCurrentProcess); R(K,"GetModuleFileNameA",3,h_GetModuleFileNameA);
    R(K,"SetEnvironmentVariableA",2,h_SetEnvironmentVariableA); R(K,"LCMapStringA",6,h_LCMapStringA); R(K,"LCMapStringW",6,h_LCMapStringW);
    R(K,"GetStringTypeA",5,h_GetStringTypeA); R(K,"GetStringTypeW",4,h_GetStringTypeW); R(K,"SetStdHandle",2,h_SetStdHandle);
    R(K,"RaiseException",4,h_RaiseException); R(K,"FlushFileBuffers",1,h_FlushFileBuffers);
    R(K,"CompareStringA",6,h_CompareStringA); R(K,"CompareStringW",6,h_CompareStringW);
    R("WINMM.dll","timeGetTime",0,h_timeGetTime);
    R("USER32.dll","wsprintfA",0,h_wsprintfA);   /* cdecl: fixed below */
    R("USER32.dll","CharUpperA",1,h_CharUpperA);
    (void)h_zero; (void)h_one; (void)scratch;
    win_register(); ddraw_register(); dinput_register(); dsound_register();
}

/* Per-second profile for the on-screen overlay. Call from one thread (the UI timer). */
int hle_prof_report(char *out,int n,int topk){
    static uint64_t pr_wall=0, pr_hle=0, pr_sl[3]={0,0,0}, pr_ns[PROF_MAX], pr_calls[PROF_MAX];
    uint64_t w=now_ns(), hle=g_prof_hle_ns, sl[3]={g_prof_sleep_ns[0],g_prof_sleep_ns[1],g_prof_sleep_ns[2]};
    uint64_t cns[PROF_MAX], ccalls[PROF_MAX]; int nt=ntab<PROF_MAX?ntab:PROF_MAX;
    for(int i=0;i<nt;i++){ cns[i]=pf_ns[i]; ccalls[i]=pf_calls[i]; }
    int first=(pr_wall==0); double dw=(double)(w-pr_wall);
    int len=0;
    if(first||dw<=0) len=snprintf(out,n,"측정 중…");
    else {
        double dh=(double)(hle-pr_hle), d0=(double)(sl[0]-pr_sl[0]), d1=(double)(sl[1]-pr_sl[1]), d2=(double)(sl[2]-pr_sl[2]);
        double ds=d0+d1+d2, di=dw-dh-ds; if(di<0) di=0;
        len=snprintf(out,n,"인터프리터 %.0f%% · HLE %.0f%% · 대기 %.0f%%\n대기: Sleep %.0f%% · 유휴틱 %.0f%% · vblank %.0f%%",
            100*di/dw,100*dh/dw,100*ds/dw,100*d0/dw,100*d1/dw,100*d2/dw);
        int used[16]; for(int k=0;k<16;k++) used[k]=-1; if(topk>16) topk=16;
        for(int k=0;k<topk;k++){
            int best=-1; uint64_t bv=0;
            for(int i=0;i<nt;i++){ int skip=0; for(int j=0;j<k;j++) if(used[j]==i) skip=1; if(skip) continue;
                uint64_t dv=cns[i]-pr_ns[i]; if(dv>bv){ bv=dv; best=i; } }
            if(best<0) break; used[k]=best;
            double ms=(double)bv/1e6*(1e9/dw), calls=(double)(ccalls[best]-pr_calls[best])*(1e9/dw);
            if(len<n) len+=snprintf(out+len,n-len,"\n%-24.24s %5.1fms/s %7.0f회/s",hle_name(best),ms,calls);
        }
    }
    pr_wall=w; pr_hle=hle; for(int i=0;i<3;i++) pr_sl[i]=sl[i];
    for(int i=0;i<nt;i++){ pr_ns[i]=cns[i]; pr_calls[i]=ccalls[i]; }
    return len;
}
