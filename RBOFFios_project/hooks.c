/* hooks.c - native replacements for hot guest loops of RBO_Ex3.exe (KR build)
 *
 * The x86 interpreter runs ~130-260 MIPS on the phone.  Three guest loops dominate the perf log:
 *
 *   0x414d10  image -> DirectDraw surface converter  (per pixel: call 0x414bc0 with 11 args, 4 byte stores)
 *   0x415060  sub-rectangle copy into a surface       (per pixel: call 0x414f60 with 10 args, 4 byte stores)
 *             => these two are the "stage loading" time (99% interpreter, no HLE)
 *   0x467130  per-frame slot scan; slots are almost all -1 so it is a pure "find next != -1" loop
 *             => 40-80% of interpreter time on the title / menu screens
 *
 * Each hook is installed by overwriting the first bytes of the guest code with `jmp <HLE thunk>`.
 * Hooks are "raw" (nargs == -2): they set ESP/EIP themselves, so they can also fall back to the original
 * code (for modes / pixel formats / address ranges they do not handle) or resume in the middle of a function.
 * The original bytes are verified first, so a different RBO_Ex3.exe build is simply left untouched.
 * RBO_NOHOOK=1 disables everything (used by test_hooks for A/B comparison).
 */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F_CONV   0x00414d10u   /* conv(surf, src, dx, dy, w, h, pal, mode)                     cdecl */
#define F_RECT   0x00415060u   /* rect(surf, src, sx, sy, stride, ?, dx, dy, w, h, mode)       cdecl */
#define F_SCAN   0x00467169u   /* loop head inside 0x467130 (esi=slot table entry, edi=index)        */
#define F_LUT    0x00416f50u   /* build 4 x 256 u16 channel LUTs from a DDSURFACEDESC2 pixel format  */
#define F_LOCK   0x00416b70u   /* lock(surf, desc) -> lpSurface or 0                                 */
#define F_UNLOCK 0x00416c00u   /* unlock(surf)                                                       */

typedef struct { uint32_t addr; uint8_t sig[8]; int len; } Sig;
static const Sig sigs[] = {
    { F_CONV, {0x81,0xec,0xb4,0x08,0x00,0x00,0x8b,0x8c}, 8 },   /* sub esp,0x8b4 ; mov ecx,[esp+..] */
    { F_RECT, {0x81,0xec,0xb8,0x08,0x00,0x00,0x8b,0x8c}, 8 },   /* sub esp,0x8b8 ; mov ecx,[esp+..] */
    { F_SCAN, {0x8b,0x56,0x0c,0x8b,0x04,0xba,0x83,0xf8}, 8 },   /* mov edx,[esi+0xc]; mov eax,[edx+edi*4]; cmp eax,.. */
};

static uint8_t saved[3][8]; static int g_installed[3];

/* guest scratch memory (allocated lazily from the guest heap; guest threads never run inside a hook) */
static uint32_t sc_desc=0, sc_lut=0;
static void scratch_init(void){
    if(!sc_desc){ sc_desc=ga_alloc(0x80,1); sc_lut=ga_alloc(0x900,1); }
}
#define LUT(ch,v) rd16(sc_lut+0x1a+0x200u*(ch)+2u*(uint32_t)(v))

static int range_ok(uint64_t a,uint64_t n){ return n==0 || (a<MEM_SIZE && a+n<=MEM_SIZE); }

/* ---- 0x414bc0 getter, modes 0..3: out[0]=R(arg7) out[1]=G(arg8) out[2]=B(arg9) out[3]=A(arg10) ---- */
static inline void get_conv(int mode,const uint8_t *src,const uint8_t *pal,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint8_t *o){
    uint32_t idx=y*w+x;
    switch(mode){
    case 0: { const uint8_t *p=src+(uint64_t)idx*4; o[0]=p[2]; o[1]=p[1]; o[2]=p[0]; o[3]=p[3]; break; }
    case 1: { uint32_t i=src[idx]; const uint8_t *p=pal+i*4; o[0]=p[0]; o[1]=p[1]; o[2]=p[2]; o[3]=i?0xff:0; break; }
    case 2: { o[0]=pal[0]; o[1]=pal[1]; o[2]=pal[2]; o[3]=src[idx]; break; }
    default:{ uint32_t i=src[idx]; const uint8_t *p=pal+i*4; o[0]=p[0]; o[1]=p[1]; o[2]=p[2]; o[3]=src[(uint64_t)(y+h)*w+x]; break; }
    }
}
/* ---- 0x414f60 getter, modes 0..2: o[0]=arg6 o[1]=arg7 o[2]=arg8 o[3]=arg9 ---- */
static inline void get_rect(int mode,const uint8_t *base,uint32_t x,uint32_t y,uint32_t stride,uint8_t *o){
    uint32_t idx=y*stride+x;
    if(mode==2){ const uint8_t *p=base+(uint64_t)idx*4; o[0]=p[0]; o[1]=p[1]; o[2]=p[2]; o[3]=p[3]; }
    else { uint32_t v=*(const uint16_t*)(base+(uint64_t)idx*2);
        if(mode==1){ o[0]=(uint8_t)((v&0xf)<<4); o[1]=(uint8_t)(((v>>4)&0xf)<<4); o[2]=(uint8_t)(((v>>8)&0xf)<<4); o[3]=(uint8_t)(((v>>12)&0xf)<<4); }
        else       { o[0]=(uint8_t)(((v)&0xf)<<3); o[1]=(uint8_t)(((v>>5)&0xf)<<3); o[2]=(uint8_t)(((v>>10)&0xf)<<3); o[3]=((v>>15)&1)?0xff:0; }
    }
}

static void ret_raw(uint32_t v){ cpu.r[EAX]=v; cpu.eip=rd32(cpu.r[ESP]); cpu.r[ESP]+=4; }   /* cdecl return */
static void raw_fallback(uint32_t resume,uint32_t frame){ cpu.r[ESP]-=frame; cpu.eip=resume; }
static void do_unlock(uint32_t surf){ uint32_t a[1]={surf}; guest_call(F_UNLOCK,1,a); }

/* ---------------- 0x414d10 ---------------- */
static void conv_impl(void){
    uint32_t surf=A(0), src=A(1), dx=A(2), dy=A(3), w=A(4), h=A(5), pal=A(6), mode=A(7);
    if(mode>3 || !range_ok(src,0)) { raw_fallback(0x414d16,0x8b4); return; }
    uint64_t npx=(uint64_t)w*h;
    uint64_t srcbytes = mode==0 ? npx*4 : mode==3 ? npx*2 : npx;
    uint64_t palbytes = mode==1||mode==3 ? 1024 : mode==2 ? 3 : 0;
    if(!range_ok(src,srcbytes)||!range_ok(pal,palbytes)|| (uint64_t)w*h>0x10000000ull){ raw_fallback(0x414d16,0x8b4); return; }
    scratch_init();
    { uint32_t a[2]={surf,sc_desc}; uint32_t lp=guest_call(F_LOCK,2,a); if(!lp){ ret_raw(0); return; }   /* same early-out as the original */
      uint32_t bpp=rd32(sc_desc+0x54), pitch=rd32(sc_desc+0x10);
      if(bpp!=16 && bpp!=32){ do_unlock(surf); ret_raw(1); return; }
      uint64_t bytes_pp=bpp/8;
      uint32_t half=(uint32_t)((int32_t)pitch/2);                      /* cdq/sub/sar */
      uint64_t first, rowstep, rowend;
      if(bpp==32){ first=(uint64_t)lp+(uint64_t)pitch*dy+(uint64_t)dx*4; rowstep=pitch; }
      else       { first=(uint64_t)lp+2ull*((uint64_t)half*dy+dx);       rowstep=2ull*half; }
      rowend = h? first+rowstep*(uint64_t)(h-1)+(uint64_t)w*bytes_pp : first;
      if(h && w && (!range_ok(first,rowend-first)||first>0xffffffffull||pitch>0x10000000u||(int32_t)pitch<0||w>0x100000u||dx>0x100000u||dy>0x100000u)){
          do_unlock(surf); raw_fallback(0x414d16,0x8b4); return; }
      const uint8_t *S=gmem+src, *P=gmem+pal;
      if(bpp==16){ uint32_t b[2]={sc_lut,sc_desc}; guest_call(F_LUT,2,b); }
      for(uint32_t r=0;r<h;r++){
          uint8_t o[4]; uint8_t *d=gmem+first+rowstep*r;
          if(bpp==32){
              for(uint32_t c=0;c<w;c++){ get_conv((int)mode,S,P,c,r,w,h,o); d[0]=o[2]; d[1]=o[1]; d[2]=o[0]; d[3]=o[3]; d+=4; }
          } else {
              for(uint32_t c=0;c<w;c++){ get_conv((int)mode,S,P,c,r,w,h,o);
                  *(uint16_t*)d=(uint16_t)(LUT(0,o[0])|LUT(1,o[1])|LUT(2,o[2])|LUT(3,o[3])); d+=2; }
          }
      }
      do_unlock(surf); ret_raw(1);
    }
}

/* ---------------- 0x415060 ---------------- */
static void rect_impl(void){
    uint32_t surf=A(0), src=A(1), sx=A(2), sy=A(3), stride=A(4), dx=A(6), dy=A(7), w=A(8), h=A(9), mode=A(10);
    if(mode>2){ raw_fallback(0x415066,0x8b8); return; }
    uint64_t bpp_src = mode==2?4:2;
    if(w>0x100000u||h>0x100000u||sx>0x1000000u||sy>0x1000000u||stride>0x1000000u||dx>0x100000u||dy>0x100000u){ raw_fallback(0x415066,0x8b8); return; }
    if(w&&h){
        uint64_t lo=((uint64_t)sy*stride+sx)*bpp_src, hi=(((uint64_t)(sy+h-1))*stride+(uint64_t)sx+w)*bpp_src;
        if(!range_ok((uint64_t)src+lo,hi-lo)){ raw_fallback(0x415066,0x8b8); return; }
    }
    scratch_init();
    uint32_t a[2]={surf,sc_desc}; uint32_t lp=guest_call(F_LOCK,2,a);
    if(!lp){ ret_raw(0); return; }                                        /* same early-out as the original */
    uint32_t bpp=rd32(sc_desc+0x54), pitch=rd32(sc_desc+0x10);
    if(bpp!=16 && bpp!=32){ do_unlock(surf); ret_raw(1); return; }
    uint32_t half=(uint32_t)((int32_t)pitch/2);
    uint64_t first, rowstep, bytes_pp=bpp/8;
    if(bpp==32){ first=(uint64_t)lp+(uint64_t)pitch*dy+(uint64_t)dx*4; rowstep=pitch; }
    else       { first=(uint64_t)lp+2ull*((uint64_t)half*dy+dx);       rowstep=2ull*half; }
    if(w&&h){
        uint64_t end=first+rowstep*(uint64_t)(h-1)+(uint64_t)w*bytes_pp;
        if(!range_ok(first,end-first)||(int32_t)pitch<0||pitch>0x10000000u){ do_unlock(surf); raw_fallback(0x415066,0x8b8); return; }
    }
    const uint8_t *S=gmem+src;
    if(bpp==16){ uint32_t b[2]={sc_lut,sc_desc}; guest_call(F_LUT,2,b); }
    for(uint32_t r=0;r<h;r++){
        uint8_t o[4]; uint8_t *d=gmem+first+rowstep*r;
        for(uint32_t c=0;c<w;c++){
            get_rect((int)mode,S,sx+c,sy+r,stride,o);
            if(bpp==32){ d[0]=o[2]; d[1]=o[1]; d[2]=o[0]; d[3]=o[3]; d+=4; }
            else { *(uint16_t*)d=(uint16_t)(LUT(0,o[0])|LUT(1,o[1])|LUT(2,o[2])|LUT(3,o[3])); d+=2; }
        }
    }
    do_unlock(surf); ret_raw(1);
}


/* ---------------- RBO_HOOKCHECK=1: shadow verification on real game data ----------------
 * For every call, snapshot the destination surface, run the native version, then restore the snapshot and run the
 * ORIGINAL guest code on the same arguments and compare the resulting surface bytes / return value.  Slow, debug only. */
static int g_check=0; static uint64_t chk_calls=0, chk_bad=0, chk_skipped=0;
static uint8_t patch_bytes[3][5];
static void check_wrap(int which){
    uint32_t addr=sigs[which].addr; int nargs=which==0?8:11;
    uint32_t args[11]; for(int i=0;i<nargs;i++) args[i]=A(i);
    uint32_t surf=args[0], dy=which==0?args[3]:args[7], h=which==0?args[5]:args[9];
    scratch_init();
    uint32_t a[2]={surf,sc_desc}; uint32_t lp=guest_call(F_LOCK,2,a);
    uint32_t pitch=rd32(sc_desc+0x10); do_unlock(surf);
    uint64_t n = lp? (uint64_t)pitch*((uint64_t)dy+h)+64 : 0;
    if(h>0x100000u||dy>0x100000u||pitch>0x10000000u||!range_ok(lp,n)) n=0;
    uint8_t *snap=malloc(n+1), *nat=malloc(n+1); memcpy(snap,gmem+lp,n);
    (which==0?conv_impl:rect_impl)();                      /* native: sets EAX/ESP/EIP (or falls back) */
    if(cpu.eip==0x414d16 || cpu.eip==0x415066 || !lp){ chk_skipped++; free(snap); free(nat); return; }
    uint32_t eax_n=cpu.r[EAX], eip_n=cpu.eip, esp_n=cpu.r[ESP];
    memcpy(nat,gmem+lp,n); memcpy(gmem+lp,snap,n);
    memcpy(gmem+addr,saved[which],5);                      /* run the original code */
    uint32_t eax_o=guest_call(addr,nargs,args);
    memcpy(gmem+addr,patch_bytes[which],5);
    chk_calls++;
    uint64_t diff=0, first=0; for(uint64_t i=0;i<n;i++) if(nat[i]!=gmem[lp+i]){ if(!diff) first=i; diff++; }
    if(diff||eax_n!=eax_o){ chk_bad++; if(chk_bad<=20) fprintf(stderr,"[hookcheck] MISMATCH %s: %llu bytes differ (first at +%llu), eax %u/%u  w=%u h=%u mode=%u\n",
        which==0?"conv":"rect",(unsigned long long)diff,(unsigned long long)first,eax_n,eax_o,args[which==0?4:8],h,args[which==0?7:10]); }
    if(chk_calls%500==0) fprintf(stderr,"[hookcheck] calls=%llu mismatches=%llu skipped(fallback)=%llu\n",(unsigned long long)chk_calls,(unsigned long long)chk_bad,(unsigned long long)chk_skipped);
    cpu.r[EAX]=eax_n; cpu.eip=eip_n; cpu.r[ESP]=esp_n;
    free(snap); free(nat);
}
static void hook_conv(void){ if(g_check) check_wrap(0); else conv_impl(); }
static void hook_rect(void){ if(g_check) check_wrap(1); else rect_impl(); }
void rbo_hookcheck_report(void){ if(g_check) fprintf(stderr,"[hookcheck] FINAL calls=%llu mismatches=%llu skipped(fallback)=%llu\n",(unsigned long long)chk_calls,(unsigned long long)chk_bad,(unsigned long long)chk_skipped); }

/* ---------------- 0x467169: skip the run of empty (-1) slots ---------------- */
static void hook_scan(void){
    uint32_t esi=cpu.r[ESI]; int32_t i=(int32_t)cpu.r[EDI];
    uint32_t slots=rd32(esi+0xc); int32_t n=(int32_t)rd32(esi+8);
    for(;;){
        uint32_t v=rd32(slots+(uint32_t)i*4);
        if(v!=0xFFFFFFFFu){ cpu.r[EDX]=slots; cpu.r[EAX]=v; cpu.r[EDI]=(uint32_t)i; cpu.eip=0x46716f; return; }
        i++;
        if(!(i<n)){ cpu.r[EDX]=slots; cpu.r[EAX]=(uint32_t)n; cpu.r[EDI]=(uint32_t)i; cpu.eip=0x467221; return; }
    }
}

static hle_fn fns[3]={hook_conv,hook_rect,hook_scan};
static const char *names[3]={"hook_conv_0x414d10","hook_rect_0x415060","hook_scan_0x467169"};

int rbo_hooks_installed(void){ return g_installed[0]+g_installed[1]+g_installed[2]; }

void rbo_uninstall_hooks(void){
    for(int i=0;i<3;i++) if(g_installed[i]){ memcpy(gmem+sigs[i].addr,saved[i],5); g_installed[i]=0; }
}
/* only=-1: all hooks; otherwise a bit mask (1=conv 2=rect 4=scan) */
void rbo_install_hooks_mask(int mask){
    for(int i=0;i<3;i++){
        if(g_installed[i] || !(mask&(1<<i))) continue;
        if(memcmp(gmem+sigs[i].addr,sigs[i].sig,sigs[i].len)){ fprintf(stderr,"[hooks] %s: code differs, not installed\n",names[i]); continue; }
        int idx=hle_register("rbo",names[i],-2,fns[i]);
        uint32_t target=hle_thunk(idx), at=sigs[i].addr;
        memcpy(saved[i],gmem+at,8);
        gmem[at]=0xE9; *(uint32_t*)(gmem+at+1)=target-(at+5); memcpy(patch_bytes[i],gmem+at,5);
        g_installed[i]=1;
    }
}
void rbo_install_hooks(void){
    const char *e=getenv("RBO_NOHOOK"); if(e&&*e&&*e!='0') return;
    { const char *c=getenv("RBO_HOOKCHECK"); g_check=(c&&*c&&*c!='0'); }
    int mask=7; const char *m=getenv("RBO_HOOKMASK"); if(m&&*m) mask=atoi(m);
    rbo_install_hooks_mask(mask);
    fprintf(stderr,"[hooks] installed: conv=%d rect=%d scan=%d\n",g_installed[0],g_installed[1],g_installed[2]);
}
