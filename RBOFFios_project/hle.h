#pragma once
#include "cpu.h"
#include <stdint.h>
typedef void (*hle_fn)(void);
typedef struct { const char *dll; const char *name; int nargs; hle_fn fn; } HleEntry;

extern int g_trace;
extern int g_cur_idx;
extern const char *g_root;       /* host dir containing DATA/ */
extern int g_max_frames;
extern int g_frames;
extern int g_fast;
void hle_vsync_advance(void);
void hle_sleep_us(uint64_t us,int src);   /* usleep that is added to the idle-time counters; src: 0=Sleep 1=idle tick 2=vblank */
int  hle_prof_report(char *out,int n,int topk);    /* human readable per-second profile (time split + top HLE functions) */
extern uint64_t g_stat_idle_us;
uint64_t hle_now_us(void);
void hle_set_event(uint32_t h);
void sched_frame_boundary(void);

#define A(i)   rd32(cpu.r[ESP]+4+4*(i))
#define RET(v) (cpu.r[EAX]=(uint32_t)(v))

int resolve_path(const char *gpath,char *out,size_t outsz);
uint32_t ga_alloc(uint32_t size, int zero);
void     ga_free(uint32_t p);
uint32_t ga_size(uint32_t p);
uint32_t ga_str(const char *s);

int  hle_register(const char *dll,const char *name,int nargs,hle_fn fn);   /* returns idx */
int  hle_lookup(const char *dll,const char *name);
uint32_t hle_thunk(int idx);
const char* hle_name(int idx);
void hle_init(void);
void hle_dispatch_impl(uint32_t idx);
uint32_t guest_call(uint32_t fn,int n,const uint32_t *args);   /* stdcall nested call */

void ddraw_register(void);
void dinput_register(void);
void dsound_register(void);
void win_register(void);
void present_frame(const uint8_t *pix,int w,int h,int pitch,int bpp,uint32_t rmask,uint32_t gmask,uint32_t bmask);
