#pragma once
/* Save-state serialisation helpers (see state.c).  Every module writes/reads its own state through the same function:
   `io->load` says which direction, so the save and load code cannot drift apart. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef struct { FILE *f; int load; int err; } StateIO;
static inline void st_raw(StateIO *io,void *p,size_t n){
    if(io->err||n==0) return;
    if(io->load){ if(fread(p,1,n,io->f)!=n) io->err=1; }
    else if(fwrite(p,1,n,io->f)!=n) io->err=1;
}
#define STX(io,x) st_raw((io),&(x),sizeof(x))
/* section marker: written on save, verified on load (a mismatch means the file and this build disagree) */
static inline void st_tag(StateIO *io,const char *t){
    char b[4]; if(io->load){ st_raw(io,b,4); if(!io->err&&memcmp(b,t,4)) io->err=2; } else { memcpy(b,t,4); st_raw(io,b,4); }
}
#define ST_ERR_IO   1   /* read/write failed */
#define ST_ERR_TAG  2   /* section marker mismatch */

void cpu_state(StateIO *io);
void sched_state(StateIO *io);
void hle_state(StateIO *io);
void win_state(StateIO *io);
void dx_state(StateIO *io);
void d3d_state(StateIO *io);
void dshow_state(StateIO *io);
void hooks_state(StateIO *io);

void d3d_state_flush(void);               /* before a save: bring the GPU-resident render target back into guest memory */
uint32_t hle_heap_top(void);
uint32_t hle_tab_count(void);
uint64_t hle_tab_hash(void);              /* hash of every registered HLE entry (dll, name, nargs): thunk addresses in guest memory are indexes into it */
int  dshow_graph_active(void);
uint32_t dshow_make_graph(void);
int  dshow_movie_active(void);
