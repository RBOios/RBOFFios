/* Cooperative green threads for the guest: switch only at blocking calls and frame boundaries. */
#include "hle.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAXTH 16
typedef struct { CPU ctx; int alive; uint32_t handle; uint32_t exit_code; int waiting; uint64_t deadline; } Thread;
static Thread th[MAXTH]={[0]={.alive=1}};
static int nth=1, cur=0;
extern int sched_multi;
int g_blocked=0;            /* set by wait calls that could not complete */
static int streak=0;

int sched_nthreads(void){ return nth; }
int sched_current(void){ return cur; }
uint64_t *sched_deadline_ptr(int *waiting){ *waiting=th[cur].waiting; return &th[cur].deadline; }
void sched_set_waiting(int w){ th[cur].waiting=w; }

static uint32_t make_teb(uint32_t stack_top,uint32_t stack_low){
    uint32_t t=ga_alloc(0x1000,1);
    wr32(t,0xFFFFFFFFu); wr32(t+4,stack_top); wr32(t+8,stack_low); wr32(t+0x18,t); wr32(t+0x30,0x00101000u);
    return t;
}
uint32_t sched_create(uint32_t start,uint32_t param){
    if(nth>=MAXTH) return 0;
    Thread *t=&th[nth]; memset(t,0,sizeof *t);
    uint32_t sz=0x40000, stk=ga_alloc(sz,1), top=stk+sz-16;
    CPU c; memset(&c,0,sizeof c); c.fcw=0x27F;
    c.fs_base=make_teb(top,stk);
    c.r[ESP]=top-8; wr32(c.r[ESP],THREAD_EXIT_MAGIC); wr32(c.r[ESP]+4,param);
    c.eip=start;
    t->ctx=c; t->alive=1; t->exit_code=259; t->handle=0x4000+nth*4;
    if(g_trace) fprintf(stderr,"[thread] created #%d start=%08x param=%08x\n",nth,start,param);
    nth++; sched_multi=1; return t->handle;
}
int sched_thread_status(uint32_t handle,uint32_t *code){
    for(int i=1;i<nth;i++) if(th[i].handle==handle){ if(code) *code=th[i].exit_code; return th[i].alive?0:1; }
    return -1;
}
void sched_set_exit_code(uint32_t code){ th[cur].exit_code=code; }
void sched_thread_exit(void){
    if(cur==0){ cpu.halted=1; return; }
    th[cur].alive=0; if(th[cur].exit_code==259) th[cur].exit_code=cpu.r[EAX];
    if(g_trace) fprintf(stderr,"[thread] #%d exited\n",cur);
    g_blocked=0; g_switch=0; streak=0;
    /* switch away permanently */
    int n=cur; do{ n=(n+1)%nth; }while(!th[n].alive&&n!=0);
    uint64_t ic=cpu.icount; cpu=th[n].ctx; cpu.icount=ic; cur=n;
}
void sched_switch(void){
    if(nth==1){ g_blocked=0; return; }
    if(g_blocked){
        streak++;
        if(streak>nth*2){ streak=0; extern void hle_idle_tick(void); hle_idle_tick(); }
    } else streak=0;
    g_blocked=0;
    int n=cur; do{ n=(n+1)%nth; }while(!th[n].alive&&n!=cur);
    if(n==cur) return;
    uint64_t ic=cpu.icount; int h=cpu.halted;
    th[cur].ctx=cpu; cpu=th[n].ctx; cpu.icount=ic; cpu.halted=h; cur=n;
}
void sched_frame_boundary(void){ if(nth>1){ g_switch=1; g_blocked=0; } }
void sched_dump(void){
    for(int i=0;i<nth;i++){
        CPU *c=(i==cur)?&cpu:&th[i].ctx;
        fprintf(stderr,"  [thread %d%s] alive=%d eip=%08x esp=%08x",i,i==cur?"*":"",th[i].alive,c->eip,c->r[ESP]);
        if(c->eip>=HLE_BASE) fprintf(stderr," in %s",hle_name((c->eip-HLE_BASE)>>2));
        fprintf(stderr," waiting=%d\n",th[i].waiting);
        int n=0; fprintf(stderr,"     stack:"); for(uint32_t a=c->r[ESP];a<c->r[ESP]+0x300&&n<8;a+=4){ uint32_t v=rd32(a); if(v>=0x401000&&v<0x483000){ fprintf(stderr," %08x",v); n++; } } fprintf(stderr,"\n");
    }
}
