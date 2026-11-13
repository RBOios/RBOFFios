#include "rbo_api.h"
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <execinfo.h>

int rbo_run(const char *exe);
void dx_set_key(int dik,int down);
extern void (*g_host_present)(void *ctx,const uint32_t *bgra,int w,int h);
extern uint64_t g_stat_present_us; extern uint64_t g_stat_idle_us; extern CPU cpu;
extern void *g_host_ctx; extern volatile int g_stop_req; extern int g_audio_ring_on;


/* Crash log: on SIGSEGV/SIGBUS/SIGILL/SIGABRT/SIGFPE write rbo_crash.txt into the game folder (signal, fault address, guest EIP, host backtrace),
   then hand the signal back to the system so the normal crash report is still produced. */
extern uint8_t *gmem;
static char crash_path[1100];
static void crash_handler(int sig,siginfo_t *si,void *uc){
    (void)uc;
    int fd=open(crash_path,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0){
        char b[400]; uint64_t fa=(uint64_t)(uintptr_t)si->si_addr, gm=(uint64_t)(uintptr_t)gmem;
        int n=snprintf(b,sizeof b,"RBO crash: signal=%d code=%d fault_addr=0x%llx\nguest_eip=0x%08x gmem=0x%llx fault_minus_gmem=0x%llx\n",
                       sig,si->si_code,(unsigned long long)fa,(unsigned)cpu.eip,(unsigned long long)gm,(unsigned long long)(fa-gm));
        if(n>0) (void)!write(fd,b,(size_t)(n<(int)sizeof b?n:(int)sizeof b-1));
        void *bt[48]; int cnt=backtrace(bt,48); backtrace_symbols_fd(bt,cnt,fd);
        close(fd);
    }
    signal(sig,SIG_DFL); raise(sig);
}
static void crash_install(const char *root){
    static int done=0; if(done) return; done=1;
    snprintf(crash_path,sizeof crash_path,"%s/rbo_crash.txt",root);
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=crash_handler; sa.sa_flags=SA_SIGINFO|SA_ONSTACK; sigemptyset(&sa.sa_mask);
    int sigs[]={SIGSEGV,SIGBUS,SIGILL,SIGABRT,SIGFPE}; for(unsigned i=0;i<sizeof sigs/sizeof sigs[0];i++) sigaction(sigs[i],&sa,NULL);
}

static pthread_t th; static volatile int running=0; static RboCallbacks cbs; static char exe_path[1024];
static void *thread_main(void *arg){
    (void)arg;
    { static char alt[1<<16]; stack_t ss; ss.ss_sp=alt; ss.ss_size=sizeof alt; ss.ss_flags=0; sigaltstack(&ss,NULL); }   /* lets the crash handler run after a stack overflow */
    int code=rbo_run(exe_path); running=0;
    if(cbs.finished) cbs.finished(cbs.ctx,code);
    return NULL;
}
int rbo_start(const char *root,const char *exe,const RboCallbacks *cb){
    if(running) return -1;
    crash_install(root);
    g_root=strdup(root); snprintf(exe_path,sizeof exe_path,"%s/%s",root,exe);
    if(cb) cbs=*cb; else memset(&cbs,0,sizeof cbs);
    g_host_present=cbs.present?(void(*)(void*,const uint32_t*,int,int))cbs.present:NULL; g_host_ctx=cbs.ctx; g_stop_req=0; g_audio_ring_on=1;
    running=1;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,8u<<20);
#ifdef __APPLE__
    /* run the emulator on the performance cores instead of letting iOS park it on efficiency cores */
    pthread_attr_set_qos_class_np(&at,QOS_CLASS_USER_INTERACTIVE,0);
#endif
    if(pthread_create(&th,&at,thread_main,NULL)!=0){ running=0; return -2; }
    return 0;
}
void rbo_stop(void){ g_stop_req=1; }
int rbo_is_running(void){ return running; }
void rbo_set_key(int dik,int down){ dx_set_key(dik,down); }
void rbo_get_stats(int *frames,double *present_ms_total){ if(frames) *frames=g_frames; if(present_ms_total) *present_ms_total=(double)g_stat_present_us/1000.0; }
void rbo_get_stats2(int *frames,double *present_ms_total,double *idle_ms_total,uint64_t *guest_instructions){
    rbo_get_stats(frames,present_ms_total); if(idle_ms_total) *idle_ms_total=(double)g_stat_idle_us/1000.0; if(guest_instructions) *guest_instructions=cpu.icount;
}
int rbo_get_prof(char *out,int n){ if(!out||n<=0) return 0; out[0]=0; int len=hle_prof_report(out,n,12); if(len<0) len=0; if(len<n-1){ len+=cpu_prof_report(out+len,n-len); } return len<n?len:n-1; }
