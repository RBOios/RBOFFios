#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>

uint32_t pe_load(const char *path);
void rbo_install_hooks(void);
void rbo_hookcheck_report(void);

void bgm_host_play(const char *path,int volume){ fprintf(stderr,"[bgm] play %s (vol %d)\n",path,volume); }
void bgm_host_stop(void){ fprintf(stderr,"[bgm] stop\n"); }
static char g_frame_dir[256]="/tmp/rbo_frames";
void present_frame_file(const char *name,const uint8_t *pix,int w,int h,int pitch,int bpp,uint32_t rm,uint32_t gm,uint32_t bm){
    FILE *f=fopen(name,"wb"); if(!f) return;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    int rs=0,gs=0,bs=0,rb=0,gb=0,bb=0; uint32_t t;
    t=rm; if(t){while(!(t&1)){t>>=1;rs++;} while(t&1){t>>=1;rb++;}}
    t=gm; if(t){while(!(t&1)){t>>=1;gs++;} while(t&1){t>>=1;gb++;}}
    t=bm; if(t){while(!(t&1)){t>>=1;bs++;} while(t&1){t>>=1;bb++;}}
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){
        uint32_t v; const uint8_t *p=pix+y*pitch+x*(bpp/8);
        v= bpp==16?*(uint16_t*)p: bpp==32?*(uint32_t*)p: (p[0]|(p[1]<<8)|(p[2]<<16));
        uint32_t r=rb?((v>>rs)&((1<<rb)-1))*255/((1<<rb)-1):0, g=gb?((v>>gs)&((1<<gb)-1))*255/((1<<gb)-1):0, b=bb?((v>>bs)&((1<<bb)-1))*255/((1<<bb)-1):0;
        fputc(r,f);fputc(g,f);fputc(b,f);
    }
    fclose(f);
}
void present_frame(const uint8_t *pix,int w,int h,int pitch,int bpp,uint32_t rm,uint32_t gm,uint32_t bm){
    static int n=0; char name[300]; snprintf(name,sizeof name,"%s/frame_%05d.ppm",g_frame_dir,g_frames); n++;
    present_frame_file(name,pix,w,h,pitch,bpp,rm,gm,bm);
}

#ifndef RBO_LIBRARY
/* ---------- checkpoint server: pause at a frame, accept commands over a FIFO, fork experiments ---------- */
void dx_script_add(int rel,int dik,int down);
void dx_script_clear(void);
void dx_set_dump_every(int n);
void dx_dump_primary(const char *path);
static int g_server=0; static int g_next_pause=0; static FILE *g_cmd=NULL;
static void reply(const char *s){ FILE *f=fopen("/tmp/rbo.out","a"); if(f){ fprintf(f,"%s\n",s); fclose(f);} }
void ctl_frame_hook(void){
    if(!g_server || g_frames<g_next_pause) return;
    char line[2048];
    for(;;){
        char msg[128]; snprintf(msg,sizeof msg,"READY frame=%d",g_frames); reply(msg);
        if(!fgets(line,sizeof line,g_cmd)){ clearerr(g_cmd); usleep(100000); continue; }
        line[strcspn(line,"\r\n")]=0;
        if(!strncmp(line,"go ",3)){ g_next_pause=g_frames+atoi(line+3); return; }
        else if(!strncmp(line,"script ",7)){
            int n=0; char *c=strdup(line+7);
            for(char *t=strtok(c,",");t;t=strtok(NULL,",")){ int r,d,dn; if(sscanf(t,"%d:%x:%d",&r,&d,&dn)==3){ dx_script_add(r,d,dn); n++; } }
            free(c); snprintf(msg,sizeof msg,"OK script %d events",n); reply(msg);
        }
        else if(!strncmp(line,"clear",5)){ dx_script_clear(); reply("OK cleared"); }
        else if(!strncmp(line,"dump ",5)){ dx_dump_primary(line+5); reply("OK dump"); }
        else if(!strncmp(line,"try ",4)){
            int nf=0,every=50; char dir[200]="/tmp/rbo_try"; sscanf(line+4,"%d %d %199s",&nf,&every,dir);
            pid_t pid=fork();
            if(pid==0){ /* child: run experiment from this exact state */
                snprintf(g_frame_dir,sizeof g_frame_dir,"%s",dir); mkdir(dir,0755);
                dx_set_dump_every(every); g_max_frames=g_frames+nf; g_next_pause=INT_MAX; return;
            }
            int st; waitpid(pid,&st,0); snprintf(msg,sizeof msg,"OK try done (dir %s)",dir); reply(msg);
        }
        else if(!strncmp(line,"quit",4)){ reply("BYE"); exit(0); }
        else { reply("ERR unknown command"); }
    }
}

#else
void ctl_frame_hook(void){}   /* no checkpoint server in the embedded build (fork() is unavailable on iOS) */
static int g_server=0; static int g_next_pause=0; static FILE *g_cmd=NULL;
#endif
static void on_alarm(int sig){
    (void)sig;
    fprintf(stderr,"[WATCHDOG] EIP=%08x ESP=%08x EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESI=%08x EDI=%08x icount=%llu frames=%d\n",
        cpu.eip,cpu.r[ESP],cpu.r[EAX],cpu.r[ECX],cpu.r[EDX],cpu.r[EBX],cpu.r[ESI],cpu.r[EDI],(unsigned long long)cpu.icount,g_frames);
    if(cpu.eip>=HLE_BASE) fprintf(stderr,"  in HLE: %s\n",hle_name((cpu.eip-HLE_BASE)>>2));
    fprintf(stderr,"  stack code addrs:");
    int n=0; for(uint32_t a=cpu.r[ESP];a<cpu.r[ESP]+0x400&&n<14;a+=4){ uint32_t v=rd32(a); if(v>=0x401000&&v<0x47a000){ fprintf(stderr," %08x",v); n++; } }
    fprintf(stderr,"\n"); { extern void sched_dump(void); sched_dump(); } fflush(stderr); _exit(3);
}
int rbo_run(const char *exe){
    gmem=mmap(NULL,MEM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    if(gmem==MAP_FAILED){ perror("mmap"); return 1; }
    cpu_init(); hle_init();
    uint32_t entry=pe_load(exe);
    rbo_install_hooks();
    /* TEB/PEB */
    cpu.fs_base=0x00100000u; wr32(0x00100000u,0xFFFFFFFFu); wr32(0x00100004u,0x1FFFF000u); wr32(0x00100008u,0x1FF00000u);
    wr32(0x00100018u,0x00100000u); wr32(0x00100030u,0x00101000u);
    cpu.r[ESP]=0x1FFFE000u; wr32(cpu.r[ESP],EXIT_MAGIC); /* return address */
    cpu.eip=entry;
    fprintf(stderr,"[start] entry=%08x\n",entry);
    cpu_run(0xFFFFFFFFu);
    rbo_hookcheck_report();
    fprintf(stderr,"[end] icount=%llu frames=%d\n",(unsigned long long)cpu.icount,g_frames);
    return 0;
}
#ifndef RBO_LIBRARY
int main(int argc,char**argv){
    const char *exe=NULL; g_root="/home/claude/game";
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--root")&&i+1<argc) g_root=argv[++i];
        else if(!strcmp(argv[i],"--frames")&&i+1<argc) g_max_frames=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--trace")) g_trace=1;
        else if(!strcmp(argv[i],"--server")&&i+1<argc){ g_server=1; g_next_pause=atoi(argv[++i]); }
        else if(!strcmp(argv[i],"--trace2")) g_trace=2;
        else if(!strcmp(argv[i],"--alarm")&&i+1<argc){ signal(SIGALRM,on_alarm); alarm(atoi(argv[++i])); }
        else exe=argv[i];
    }
    if(!exe){ fprintf(stderr,"usage: rboemu [--root dir] [--frames N] [--trace] exe\n"); return 1; }
    { const char *fd=getenv("RBO_FRAME_DIR"); if(fd) snprintf(g_frame_dir,sizeof g_frame_dir,"%s",fd); char cmd[300]; snprintf(cmd,sizeof cmd,"mkdir -p %s",g_frame_dir); system(cmd); }
    if(g_server){ unlink("/tmp/rbo.cmd"); mkfifo("/tmp/rbo.cmd",0600); unlink("/tmp/rbo.out"); g_cmd=fopen("/tmp/rbo.cmd","r+"); }
    return rbo_run(exe);
}
#endif
