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
 * Two more hooks replace the float matrix-multiply routines that dominate the interpreter once the earlier loops are gone:
 *   0x412b40  4x4 matrix multiply  mm4(dst, A, B)   (all 16 elements, x87)
 *   0x412f00  4x3 style multiply   mm3(dst, A, B)   (9 elements, x87)
 *             => ~35% of the sampled guest instructions in a busy stage.  hooks_mm.h is generated from the exe's own code by
 *                gen_mm.py (same double-precision operations in the same order, loads and stores interleaved like the original,
 *                so the result is bit-identical even when dst overlaps A or B).
 * RBO_NOHOOK=1 disables everything (used by test_hooks for A/B comparison).
 */
#include "hle.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define F_CONV   0x00414d10u   /* conv(surf, src, dx, dy, w, h, pal, mode)                     cdecl */
#define F_RECT   0x00415060u   /* rect(surf, src, sx, sy, stride, ?, dx, dy, w, h, mode)       cdecl */
#define F_SCAN   0x00467169u   /* loop head inside 0x467130 (esi=slot table entry, edi=index)        */
#define F_MM4    0x00412b40u   /* mm4(dst, A, B)  cdecl, 4x4 float matrices                           */
#define F_MM3    0x00412f00u   /* mm3(dst, A, B)  cdecl                                               */
#define F_VX    0x004121c0u   /* xform(count, dst, src)  cdecl: vertex transform loop (hooks_vx.h)  */
#define F_FTOL  0x0047c4ecu   /* _ftol: st0 -> edx:eax, truncating                                  */
#define NH 7                   /* hooks: 0 conv, 1 rect, 2 scan, 3 mm4, 4 mm3, 5 vx, 6 ftol */
#define F_LUT    0x00416f50u   /* build 4 x 256 u16 channel LUTs from a DDSURFACEDESC2 pixel format  */
#define F_LOCK   0x00416b70u   /* lock(surf, desc) -> lpSurface or 0                                 */
#define F_UNLOCK 0x00416c00u   /* unlock(surf)                                                       */

typedef struct { uint32_t addr; uint8_t sig[8]; int len; } Sig;
static const Sig sigs[] = {
    { F_CONV, {0x81,0xec,0xb4,0x08,0x00,0x00,0x8b,0x8c}, 8 },   /* sub esp,0x8b4 ; mov ecx,[esp+..] */
    { F_RECT, {0x81,0xec,0xb8,0x08,0x00,0x00,0x8b,0x8c}, 8 },   /* sub esp,0x8b8 ; mov ecx,[esp+..] */
    { F_SCAN, {0x8b,0x56,0x0c,0x8b,0x04,0xba,0x83,0xf8}, 8 },   /* mov edx,[esi+0xc]; mov eax,[edx+edi*4]; cmp eax,.. */
    { F_MM4,  {0}, 0 },                                          /* verified against the complete function body (hooks_mm.h) */
    { F_MM3,  {0}, 0 },
    { F_VX,   {0}, 0 },
    { F_FTOL, {0}, 0 },
};

static uint8_t saved[NH][8]; static int g_installed[NH];

/* guest scratch memory (allocated lazily from the guest heap; guest threads never run inside a hook) */
static uint32_t sc_desc=0, sc_lut=0;
static void scratch_init(void){
    if(!sc_desc){ sc_desc=ga_alloc(0x80,1); sc_lut=ga_alloc(0x900,1); }
}
void hooks_state(StateIO *io){ st_tag(io,"HOK0"); STX(io,sc_desc); STX(io,sc_lut); }
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
static uint8_t patch_bytes[NH][5];
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

/* ---------------- 0x412b40 / 0x412f00: float matrix multiplies ---------------- */
#include "hooks_mm.h"
#include "hooks_vx.h"
static const uint8_t *const bodies[NH]={0,0,0,mm4_body,mm3_body,vx_body,ftol_body};
static const int blen[NH]={0,0,0,(int)sizeof mm4_body,(int)sizeof mm3_body,(int)sizeof vx_body,(int)sizeof ftol_body};
static int mm_range_ok(uint32_t p){ return (uint64_t)p+0x40<=MEM_SIZE; }
/* cdecl (dst, A, B): the original prologue loads eax=A ecx=B edx=dst and leaves them there; the x87 stack is balanced at the ret */
static void mm_common(int which){
    uint32_t sp=cpu.r[ESP], D=rd32(sp+4), A=rd32(sp+8), B=rd32(sp+12);
    cpu.r[EAX]=A; cpu.r[ECX]=B; cpu.r[EDX]=D;
    if(!mm_range_ok(A)||!mm_range_ok(B)||!mm_range_ok(D)){ cpu.eip=sigs[which].addr+12; return; }   /* resume the original code after its prologue (it reports the bad access itself) */
    if(which==3) mm4_native(A,B,D); else mm3_native(A,B,D);
    cpu.eip=rd32(sp); cpu.r[ESP]=sp+4;
}
static void check_wrap_mm(int which){
    uint32_t addr=sigs[which].addr, sp=cpu.r[ESP], D=rd32(sp+4), A=rd32(sp+8), B=rd32(sp+12);
    if(!mm_range_ok(A)||!mm_range_ok(B)||!mm_range_ok(D)){ chk_skipped++; mm_common(which); return; }
    uint8_t sn[3][0x40], nt[3][0x40]; const uint32_t P[3]={A,B,D};
    for(int k=0;k<3;k++) memcpy(sn[k],gmem+P[k],0x40);
    mm_common(which);                                              /* native */
    uint32_t eip_n=cpu.eip, esp_n=cpu.r[ESP];
    for(int k=0;k<3;k++) memcpy(nt[k],gmem+P[k],0x40);
    for(int k=0;k<3;k++) memcpy(gmem+P[k],sn[k],0x40);             /* back to the state before the call */
    memcpy(gmem+addr,saved[which],5);                              /* original code */
    uint32_t args[3]={D,A,B}; uint32_t eax_o=guest_call(addr,3,args);
    memcpy(gmem+addr,patch_bytes[which],5);
    chk_calls++; int bad=(eax_o!=A);
    for(int k=0;k<3&&!bad;k++) if(memcmp(nt[k],gmem+P[k],0x40)) bad=1;
    if(bad){ chk_bad++; if(chk_bad<=20) fprintf(stderr,"[hookcheck] MISMATCH %s dst=%08x A=%08x B=%08x\n",which==3?"mm4":"mm3",D,A,B); }
    if(chk_calls%500==0) fprintf(stderr,"[hookcheck] calls=%llu mismatches=%llu skipped(fallback)=%llu\n",(unsigned long long)chk_calls,(unsigned long long)chk_bad,(unsigned long long)chk_skipped);
    cpu.r[EAX]=A; cpu.r[ECX]=B; cpu.r[EDX]=D; cpu.eip=eip_n; cpu.r[ESP]=esp_n;
}

/* RBO_HOOKCHECK for vx / ftol: native first, then the original guest code on the same input; everything the game can observe must match */
static void hook_vx_plain(void);
static void check_wrap_vx(void){
    uint32_t sp=cpu.r[ESP]; int32_t n=(int32_t)rd32(sp+4); uint32_t D=rd32(sp+8), S=rd32(sp+12);
    uint64_t span = n>0 ? (uint64_t)(n-1)*0x20u+12u : 0;
    if(n<=0||!range_ok(D,span)||span>0x1000000u){ chk_skipped++; hook_vx_plain(); return; }
    uint8_t *snap=malloc(span), *nat=malloc(span); memcpy(snap,gmem+D,span);
    uint32_t r0[8]; memcpy(r0,cpu.r,sizeof r0); uint16_t cc0=cpu.fsw_cc;
    hook_vx_plain();
    if(cpu.eip==0x4121c5u){ chk_skipped++; free(snap); free(nat); return; }                      /* fell back to the original code by itself */
    uint32_t eip_n=cpu.eip, esp_n=cpu.r[ESP], eax_n=cpu.r[EAX], ecx_n=cpu.r[ECX], edx_n=cpu.r[EDX], esi_n=cpu.r[ESI]; uint16_t cc_n=cpu.fsw_cc;
    memcpy(nat,gmem+D,span); memcpy(gmem+D,snap,span);
    memcpy(gmem+F_VX,saved[5],5); cpu.fsw_cc=cc0;
    uint32_t args[3]={(uint32_t)n,D,S}; uint32_t esi0=cpu.r[ESI]; uint32_t eax_o=guest_call(F_VX,3,args);
    memcpy(gmem+F_VX,patch_bytes[5],5);
    chk_calls++; int bad = memcmp(nat,gmem+D,span)||eax_o!=eax_n||ecx_n!=cpu.r[ECX]||edx_n!=cpu.r[EDX]||esi_n!=cpu.r[ESI]||cc_n!=cpu.fsw_cc||esi0!=cpu.r[ESI];
    if(bad){ chk_bad++; if(chk_bad<=20) fprintf(stderr,"[hookcheck] MISMATCH vx n=%d dst=%08x src=%08x\n",n,D,S); }
    if(chk_calls%500==0) fprintf(stderr,"[hookcheck] calls=%llu mismatches=%llu skipped(fallback)=%llu\n",(unsigned long long)chk_calls,(unsigned long long)chk_bad,(unsigned long long)chk_skipped);
    memcpy(cpu.r,r0,sizeof r0); cpu.r[EAX]=eax_n; cpu.r[ECX]=ecx_n; cpu.r[EDX]=edx_n; cpu.r[ESI]=esi_n; cpu.r[ESP]=esp_n; cpu.eip=eip_n; cpu.fsw_cc=cc_n;
    free(snap); free(nat);
}
static void check_wrap_ftol(void){
    uint32_t E=cpu.r[ESP]; double v=cpu.st[cpu.top]; int top0=cpu.top; uint32_t ebp0=cpu.r[EBP];
    ftol_hook();
    if(cpu.eip==0x47c4f2u){ chk_skipped++; return; }
    uint32_t eax_n=cpu.r[EAX], edx_n=cpu.r[EDX], eip_n=cpu.eip, esp_n=cpu.r[ESP]; uint16_t fcw_n=cpu.fcw; int top_n=cpu.top;
    cpu.top=top0; cpu.st[cpu.top]=v; cpu.r[ESP]=E; cpu.r[EBP]=ebp0;
    memcpy(gmem+F_FTOL,saved[6],5);
    uint32_t eax_o=guest_call(F_FTOL,0,NULL); uint32_t edx_o=cpu.r[EDX];
    memcpy(gmem+F_FTOL,patch_bytes[6],5);
    chk_calls++;
    if(eax_o!=eax_n||edx_o!=edx_n||fcw_n!=cpu.fcw||top_n!=cpu.top||cpu.r[EBP]!=ebp0){ chk_bad++; if(chk_bad<=20) fprintf(stderr,"[hookcheck] MISMATCH ftol v=%g: %08x:%08x vs %08x:%08x\n",v,edx_n,eax_n,edx_o,eax_o); }
    cpu.r[EAX]=eax_n; cpu.r[EDX]=edx_n; cpu.eip=eip_n; cpu.r[ESP]=esp_n; cpu.top=top_n; cpu.r[EBP]=ebp0;
}
static void hook_vx_plain(void){ uint32_t sp=cpu.r[ESP]; int32_t n=(int32_t)rd32(sp+4); uint32_t D=rd32(sp+8), S=rd32(sp+12);
    if(!vx_native(n,D,S)){ vx_fallback(); return; }
    if(n<=0){ cpu.ZF=(n==0); cpu.SF=(n<0); cpu.PF=!__builtin_parity((uint32_t)n&0xff); cpu.CF=0; cpu.OF=0; cpu.AF=0; }
    cpu.eip=rd32(sp); cpu.r[ESP]=sp+4; }
static void hook_vx(void){ if(g_check) check_wrap_vx(); else hook_vx_plain(); }
static void hook_ftol(void){ if(g_check) check_wrap_ftol(); else ftol_hook(); }
static void hook_mm4(void){ if(g_check) check_wrap_mm(3); else mm_common(3); }
static void hook_mm3(void){ if(g_check) check_wrap_mm(4); else mm_common(4); }

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

/* One HLE table entry serves three hooks: the thunk of hook_conv at +0, and +1 / +2 for mm4 / mm3 (the dispatcher uses
 * (eip-HLE_BASE)>>2 as the index, so the low two bits of EIP are free).  The HLE table therefore keeps its old size and hash,
 * and save states made before mm4/mm3 existed still load. */
static void hook_mux(void){
    switch(cpu.eip&3){ case 0: hook_conv(); break; case 1: hook_mm4(); break; case 2: hook_mm3(); break; default: hook_vx(); break; }
}
/* the scan hook's thunk is shared the same way: +0 = scan, +1 = _ftol (so vx and ftol did not add HLE table entries either) */
static void hook_mux2(void){ if(cpu.eip&3) hook_ftol(); else hook_scan(); }
static hle_fn fns[NH]={hook_conv,hook_rect,hook_mux2,hook_mm4,hook_mm3,hook_vx,hook_ftol};
static int mux_idx=-1, mux2_idx=-1;
static const char *names[NH]={"hook_conv_0x414d10","hook_rect_0x415060","hook_scan_0x467169","hook_mm4_0x412b40","hook_mm3_0x412f00","hook_vx_0x4121c0","hook_ftol_0x47c4ec"};

int rbo_hooks_installed(void){ int n=0; for(int i=0;i<NH;i++) n+=g_installed[i]; return n; }

void rbo_uninstall_hooks(void){
    for(int i=0;i<NH;i++) if(g_installed[i]){ memcpy(gmem+sigs[i].addr,saved[i],5); g_installed[i]=0; }
}
/* only=-1: all hooks; otherwise a bit mask (1=conv 2=rect 4=scan 8=mm4 16=mm3) */
void rbo_install_hooks_mask(int mask){
    for(int i=0;i<NH;i++){
        if(g_installed[i] || !(mask&(1<<i))) continue;
        if(bodies[i] ? memcmp(gmem+sigs[i].addr,bodies[i],blen[i]) : memcmp(gmem+sigs[i].addr,sigs[i].sig,sigs[i].len)){ fprintf(stderr,"[hooks] %s: code differs, not installed\n",names[i]); continue; }
        uint32_t at=sigs[i].addr, target;
        if(i==0||i==3||i==4||i==5){ if(mux_idx<0) mux_idx=hle_register("rbo",names[0],-2,hook_mux); target=hle_thunk(mux_idx)+(i==0?0u:i==5?3u:(uint32_t)(i-2)); }
        else if(i==2||i==6){ if(mux2_idx<0) mux2_idx=hle_register("rbo",names[2],-2,hook_mux2); target=hle_thunk(mux2_idx)+(i==2?0u:1u); }
        else target=hle_thunk(hle_register("rbo",names[i],-2,fns[i]));
        memcpy(saved[i],gmem+at,8);
        gmem[at]=0xE9; *(uint32_t*)(gmem+at+1)=target-(at+5); memcpy(patch_bytes[i],gmem+at,5);
        g_installed[i]=1;
    }
}
/* After a save state is loaded the guest code is whatever was saved: a state made before a hook existed has the original bytes there.
 * Re-apply every hook of this process whose function is still pristine (called from state.c after a successful load). */
void rbo_hooks_resync(void){
    for(int i=0;i<NH;i++){
        if(!g_installed[i]) continue;
        uint32_t at=sigs[i].addr;
        if(!memcmp(gmem+at,patch_bytes[i],5)) continue;                       /* already patched */
        int pristine = bodies[i] ? !memcmp(gmem+at,bodies[i],blen[i]) : !memcmp(gmem+at,sigs[i].sig,sigs[i].len);
        if(pristine) memcpy(gmem+at,patch_bytes[i],5);
    }
}
void rbo_install_hooks(void){
    const char *e=getenv("RBO_NOHOOK"); if(e&&*e&&*e!='0') return;
    { const char *c=getenv("RBO_HOOKCHECK"); g_check=(c&&*c&&*c!='0'); }
    int mask=(1<<NH)-1; const char *m=getenv("RBO_HOOKMASK"); if(m&&*m) mask=atoi(m);
    rbo_install_hooks_mask(mask);
    fprintf(stderr,"[hooks] installed: conv=%d rect=%d scan=%d mm4=%d mm3=%d vx=%d ftol=%d\n",g_installed[0],g_installed[1],g_installed[2],g_installed[3],g_installed[4],g_installed[5],g_installed[6]);
}
