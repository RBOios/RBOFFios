// RBO port prototype: tiny x86-32 interpreter (user mode, flat memory)
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MEM_SIZE   0x20000000u      /* 512MB guest address space */
#define HLE_BASE   0xF0000000u      /* EIP >= this => host-implemented function */
#define EXIT_MAGIC 0xF8000000u
#define THREAD_EXIT_MAGIC 0xF9000000u      /* return address meaning "guest function returned" */

typedef struct CPU {
    uint32_t r[8];                  /* EAX ECX EDX EBX ESP EBP ESI EDI */
    uint32_t eip;
    uint8_t  CF, PF, AF, ZF, SF, OF, DF;
    uint32_t fs_base;
    /* x87 */
    double   st[8];
    int      top;
    uint16_t fcw;
    uint16_t fsw_cc;                /* C0,C1,C2,C3 as bits 8,9,10,14 */
    int      halted;
    uint64_t icount;
} CPU;

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

extern CPU cpu;
extern uint32_t g_code_lo, g_code_hi;
extern uint8_t *gmem;

void cpu_init(void);
/* run until EIP == stop_eip (used for nested guest calls) or process exit */
void cpu_run(uint32_t stop_eip);

/* memory helpers (bounds checked, abort on fault) */
void     gfault(const char *what, uint32_t addr);
static inline uint8_t  rd8 (uint32_t a){ if(a>=MEM_SIZE) gfault("read8",a);  return gmem[a]; }
static inline uint16_t rd16(uint32_t a){ if(a+1>=MEM_SIZE) gfault("read16",a); return *(uint16_t*)(gmem+a); }
static inline uint32_t rd32(uint32_t a){ if(a+3>=MEM_SIZE) gfault("read32",a); return *(uint32_t*)(gmem+a); }
static inline void wr8 (uint32_t a,uint8_t v ){ if(a>=MEM_SIZE) gfault("write8",a);  gmem[a]=v; }
static inline void wr16(uint32_t a,uint16_t v){ if(a+1>=MEM_SIZE) gfault("write16",a); *(uint16_t*)(gmem+a)=v; }
static inline void wr32(uint32_t a,uint32_t v){ if(a+3>=MEM_SIZE) gfault("write32",a); *(uint32_t*)(gmem+a)=v; }

/* HLE hook: called when EIP >= HLE_BASE. Must set up return itself. */
extern void (*hle_dispatch)(uint32_t idx);
extern int g_switch, g_nest;      /* scheduler: request a thread switch / nested guest-call depth */
void sched_switch(void);
void sched_thread_exit(void);
int cpu_prof_report(char *out,int n);   /* hottest guest code blocks of the last second (address + code bytes), for the perf log */
