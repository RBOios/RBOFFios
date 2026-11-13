#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

CPU cpu;
uint32_t g_code_lo=0x401000u, g_code_hi=0x47a000u;
uint8_t *gmem;
void (*hle_dispatch)(uint32_t idx);
int g_switch=0, g_nest=0, sched_multi=0;
static uint64_t next_slice=0;

void gfault(const char *what, uint32_t addr){
    fprintf(stderr,"[FAULT] %s at 0x%08x  EIP=0x%08x ESP=0x%08x icount=%llu\n",what,addr,cpu.eip,cpu.r[ESP],(unsigned long long)cpu.icount);
    fflush(stderr);
    abort();
}
static uint32_t ring[256]; static int ringp=0;
static void dump_ring(void){
    int n=255; /* skip trailing sequential +2 walk through zero memory */
    while(n>0 && ring[(ringp-1-(255-n)-0)&255]==0) n--;
    int end=ringp-1; int k=0;
    while(k<250 && (uint32_t)(ring[(end)&255]) == (uint32_t)(ring[(end-1)&255])+2) { end--; k++; }
    fprintf(stderr,"  (skipped %d zero-walk steps) EIPs before:",k);
    for(int i=30;i>=0;i--) fprintf(stderr," %08x",ring[(end-i)&255]);
    fprintf(stderr,"\n");
    fprintf(stderr,"  regs EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",cpu.r[0],cpu.r[1],cpu.r[2],cpu.r[3],cpu.r[4],cpu.r[5],cpu.r[6],cpu.r[7]);
}
static void __attribute__((noinline,cold)) unimpl(const char *what, uint32_t eip0, int a, int b){
    fprintf(stderr,"[UNIMPL] %s opcode %02x %02x at 0x%08x\n",what,a,b,eip0);
    dump_ring(); fflush(stderr); abort();
}

void cpu_init(void){
    memset(&cpu,0,sizeof cpu);
    cpu.fcw=0x27F;
}

/* ---------- register access ---------- */
static inline uint32_t getreg(int sz,int n){
    if(sz==4) return cpu.r[n];
    if(sz==2) return cpu.r[n]&0xFFFF;
    return n<4 ? (cpu.r[n]&0xFF) : ((cpu.r[n-4]>>8)&0xFF);
}
static inline void setreg(int sz,int n,uint32_t v){
    if(sz==4) cpu.r[n]=v;
    else if(sz==2) cpu.r[n]=(cpu.r[n]&0xFFFF0000u)|(v&0xFFFF);
    else if(n<4) cpu.r[n]=(cpu.r[n]&0xFFFFFF00u)|(v&0xFF);
    else cpu.r[n-4]=(cpu.r[n-4]&0xFFFF00FFu)|((v&0xFF)<<8);
}
static inline uint32_t rdm(int sz,uint32_t a){ return sz==4?rd32(a):sz==2?rd16(a):rd8(a); }
static inline void wrm(int sz,uint32_t a,uint32_t v){ if(sz==4)wr32(a,v); else if(sz==2)wr16(a,v); else wr8(a,v); }
static inline uint32_t szmask(int sz){ return sz==4?0xFFFFFFFFu:sz==2?0xFFFFu:0xFFu; }
static inline uint32_t szsign(int sz){ return sz==4?0x80000000u:sz==2?0x8000u:0x80u; }
static inline void push32(uint32_t v){ cpu.r[ESP]-=4; wr32(cpu.r[ESP],v); }
static inline uint32_t pop32(void){ uint32_t v=rd32(cpu.r[ESP]); cpu.r[ESP]+=4; return v; }
static inline void push16(uint16_t v){ cpu.r[ESP]-=2; wr16(cpu.r[ESP],v); }
static inline uint16_t pop16(void){ uint16_t v=rd16(cpu.r[ESP]); cpu.r[ESP]+=2; return v; }

#define PAR2(n) n, n^1, n^1, n
#define PAR4(n) PAR2(n), PAR2(n^1), PAR2(n^1), PAR2(n)
#define PAR6(n) PAR4(n), PAR4(n^1), PAR4(n^1), PAR4(n)
static const uint8_t par_tab[256]={ PAR6(1), PAR6(0), PAR6(0), PAR6(1) };   /* 1 when the low byte has an even number of set bits */
static inline int parity8(uint32_t v){ return par_tab[v&0xFF]; }
static inline void setszp(uint32_t res,int sz){
    res&=szmask(sz);
    cpu.ZF = res==0; cpu.SF = (res&szsign(sz))!=0; cpu.PF = parity8(res);
}

/* ---------- flags / ALU ---------- */
static uint32_t alu(int op,uint32_t a,uint32_t b,int sz){
    uint32_t m=szmask(sz), s=szsign(sz);
    a&=m; b&=m;
    uint64_t r; uint32_t res;
    switch(op){
    case 0: case 2: { /* add/adc */
        uint32_t c = (op==2)?cpu.CF:0;
        r=(uint64_t)a+b+c; res=(uint32_t)r&m;
        cpu.CF = r>m; cpu.OF = ((a^res)&(b^res)&s)!=0; cpu.AF=((a^b^res)&0x10)!=0;
        setszp(res,sz); return res; }
    case 3: case 5: case 7: { /* sbb/sub/cmp */
        uint32_t c = (op==3)?cpu.CF:0;
        r=(uint64_t)a-b-c; res=(uint32_t)r&m;
        cpu.CF = ((uint64_t)a < (uint64_t)b+c); cpu.OF = ((a^b)&(a^res)&s)!=0; cpu.AF=((a^b^res)&0x10)!=0;
        setszp(res,sz); return op==7?a:res; }
    case 1: res=a|b; break;
    case 4: res=a&b; break;
    default: res=a^b; break; /* 6 */
    }
    cpu.CF=0; cpu.OF=0; cpu.AF=0; setszp(res,sz); return res;
}

/* 32-bit ALU fast path: same flag results as alu(op,a,b,4), without the size-generic masking */
#define SZP32(res) do{ cpu.ZF=((res)==0); cpu.SF=(uint8_t)((res)>>31); cpu.PF=(uint8_t)parity8(res); }while(0)
static inline __attribute__((always_inline)) uint32_t alu32(int op,uint32_t a,uint32_t b){
    uint32_t res;
    switch(op){
    case 0: res=a+b; cpu.CF=(res<a); cpu.OF=(uint8_t)(((a^res)&(b^res))>>31); cpu.AF=(uint8_t)(((a^b^res)>>4)&1); SZP32(res); return res;
    case 2: { uint64_t r=(uint64_t)a+b+cpu.CF; res=(uint32_t)r; cpu.CF=(uint8_t)(r>>32); cpu.OF=(uint8_t)(((a^res)&(b^res))>>31); cpu.AF=(uint8_t)(((a^b^res)>>4)&1); SZP32(res); return res; }
    case 5: res=a-b; cpu.CF=(a<b); cpu.OF=(uint8_t)(((a^b)&(a^res))>>31); cpu.AF=(uint8_t)(((a^b^res)>>4)&1); SZP32(res); return res;
    case 7: res=a-b; cpu.CF=(a<b); cpu.OF=(uint8_t)(((a^b)&(a^res))>>31); cpu.AF=(uint8_t)(((a^b^res)>>4)&1); SZP32(res); return a;
    case 3: { uint32_t c=cpu.CF; res=a-b-c; cpu.CF=((uint64_t)a<(uint64_t)b+c); cpu.OF=(uint8_t)(((a^b)&(a^res))>>31); cpu.AF=(uint8_t)(((a^b^res)>>4)&1); SZP32(res); return res; }
    case 1: res=a|b; break;
    case 4: res=a&b; break;
    default: res=a^b; break;
    }
    cpu.CF=0; cpu.OF=0; cpu.AF=0; SZP32(res); return res;
}
#define ALU(op,a,b,sz) ((sz)==4 ? alu32((op),(a),(b)) : alu((op),(a),(b),(sz)))
static inline uint32_t incdec(int dec,uint32_t a,int sz){
    uint8_t cf=cpu.CF; uint32_t r = sz==4 ? (dec? alu32(5,a,1) : alu32(0,a,1)) : alu(dec?5:0,a,1,sz); cpu.CF=cf; return r;
}
static int cond(int cc){
    int r;
    switch(cc>>1){
    case 0: r=cpu.OF; break;
    case 1: r=cpu.CF; break;
    case 2: r=cpu.ZF; break;
    case 3: r=cpu.CF||cpu.ZF; break;
    case 4: r=cpu.SF; break;
    case 5: r=cpu.PF; break;
    case 6: r=cpu.SF!=cpu.OF; break;
    default: r=cpu.ZF||(cpu.SF!=cpu.OF); break;
    }
    return (cc&1)?!r:r;
}
static uint32_t get_eflags(void){
    return 2u|cpu.CF|(cpu.PF<<2)|(cpu.AF<<4)|(cpu.ZF<<6)|(cpu.SF<<7)|(1u<<9)|(cpu.DF<<10)|(cpu.OF<<11);
}
static void set_eflags(uint32_t f){
    cpu.CF=f&1; cpu.PF=(f>>2)&1; cpu.AF=(f>>4)&1; cpu.ZF=(f>>6)&1; cpu.SF=(f>>7)&1; cpu.DF=(f>>10)&1; cpu.OF=(f>>11)&1;
}

static uint32_t shiftop(int op,uint32_t v,uint32_t cnt,int sz){
    uint32_t m=szmask(sz), s=szsign(sz); int bits=sz*8;
    v&=m;
    if(op==2||op==3){ cnt&=31; cnt%= (bits+1); } else cnt&=31;
    if(cnt==0) return v;
    uint32_t res=v;
    switch(op){
    case 0: { uint32_t c=cnt%bits; res=((v<<c)|(v>>(bits-c)))&m; if(c==0)res=v; cpu.CF=res&1; cpu.OF=((res&s)!=0)^cpu.CF; return res; }
    case 1: { uint32_t c=cnt%bits; res=((v>>c)|(v<<(bits-c)))&m; if(c==0)res=v; cpu.CF=(res&s)!=0; cpu.OF=((res&s)!=0)^((res&(s>>1))!=0); return res; }
    case 2: { uint64_t x=((uint64_t)cpu.CF<<bits)|v; uint64_t full=((uint64_t)1<<(bits+1))-1;
              x=((x<<cnt)|(x>>(bits+1-cnt)))&full; res=(uint32_t)x&m; cpu.CF=(x>>bits)&1; cpu.OF=((res&s)!=0)^cpu.CF; return res; }
    case 3: { uint64_t x=((uint64_t)cpu.CF<<bits)|v; uint64_t full=((uint64_t)1<<(bits+1))-1;
              x=((x>>cnt)|(x<<(bits+1-cnt)))&full; res=(uint32_t)x&m; cpu.CF=(x>>bits)&1; cpu.OF=((res&s)!=0)^((res&(s>>1))!=0); return res; }
    case 4: case 6:
        cpu.CF = cnt<=(uint32_t)bits ? ((v>>(bits-cnt))&1) : 0;
        res = cnt>=(uint32_t)bits?0:(v<<cnt)&m;
        cpu.OF = ((res&s)!=0)^cpu.CF; setszp(res,sz); return res;
    case 5:
        cpu.CF = (v>>(cnt-1))&1; res = cnt>=(uint32_t)bits?0:(v>>cnt);
        cpu.OF = (v&s)!=0; setszp(res,sz); return res;
    default: { /* sar */
        int32_t sv = sz==4?(int32_t)v: sz==2?(int16_t)v:(int8_t)v;
        cpu.CF = (cnt>=32)? (sv<0) : (((int64_t)sv>>(cnt-1))&1);
        res = (uint32_t)((cnt>=32)?(sv<0?-1:0):(sv>>cnt))&m;
        cpu.OF=0; setszp(res,sz); return res; }
    }
}

/* ---------- decode state ---------- */
static int opsz;        /* 2 or 4 */
static uint32_t segbase;
static int mod_, reg_, rm_;
static uint32_t ea_;

/* instruction fetch: step() only runs with EIP inside [g_code_lo,g_code_hi) (checked in cpu_run), far below MEM_SIZE, so no bounds check needed */
static inline uint8_t  f8(void){ return gmem[cpu.eip++]; }
static inline uint16_t f16(void){ uint16_t v; memcpy(&v,gmem+cpu.eip,2); cpu.eip+=2; return v; }
static inline uint32_t f32(void){ uint32_t v; memcpy(&v,gmem+cpu.eip,4); cpu.eip+=4; return v; }

static inline __attribute__((always_inline)) void modrm(void){
    uint8_t m=f8(); mod_=m>>6; reg_=(m>>3)&7; rm_=m&7;
    if(mod_==3) return;
    uint32_t ea=0;
    if(rm_==4){
        uint8_t sib=f8(); int sc=sib>>6, idx=(sib>>3)&7, base=sib&7;
        if(base==5 && mod_==0){ ea=f32(); } else ea=cpu.r[base];
        if(idx!=4) ea += cpu.r[idx]<<sc;
    } else if(rm_==5 && mod_==0){
        ea=f32();
    } else ea=cpu.r[rm_];
    if(mod_==1) ea += (int32_t)(int8_t)f8();
    else if(mod_==2) ea += f32();
    ea_ = ea + segbase;
}
static inline uint32_t get_rm(int sz){ return mod_==3?getreg(sz,rm_):rdm(sz,ea_); }
static inline void set_rm(int sz,uint32_t v){ if(mod_==3)setreg(sz,rm_,v); else wrm(sz,ea_,v); }

/* ---------- x87 ---------- */
#define ST(i) cpu.st[(cpu.top+(i))&7]
static inline void fpush(double v){ cpu.top=(cpu.top-1)&7; cpu.st[cpu.top]=v; }
static inline double fpop(void){ double v=cpu.st[cpu.top]; cpu.top=(cpu.top+1)&7; return v; }
static void fcmp_set(double a,double b){
    cpu.fsw_cc &= ~((1<<8)|(1<<9)|(1<<10)|(1<<14));
    if(isnan(a)||isnan(b)) cpu.fsw_cc |= (1<<8)|(1<<10)|(1<<14);
    else if(a>b) {}
    else if(a<b) cpu.fsw_cc |= (1<<8);
    else cpu.fsw_cc |= (1<<14);
}
static void fcomi_set(double a,double b){
    cpu.OF=0; cpu.SF=0; cpu.AF=0;
    if(isnan(a)||isnan(b)){cpu.ZF=1;cpu.PF=1;cpu.CF=1;}
    else if(a>b){cpu.ZF=0;cpu.PF=0;cpu.CF=0;}
    else if(a<b){cpu.ZF=0;cpu.PF=0;cpu.CF=1;}
    else {cpu.ZF=1;cpu.PF=0;cpu.CF=0;}
}
static double fround(double v){
    switch((cpu.fcw>>10)&3){
    case 0: return nearbyint(v);   /* default rounding mode: nearest-even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static double farith(int op,double a,double b){ /* a op b */
    switch(op){ case 0: return a+b; case 1: return a*b; case 4: return a-b; case 5: return b-a; case 6: return a/b; default: return b/a; }
}
static uint16_t get_fsw(void){ return cpu.fsw_cc | (uint16_t)(cpu.top<<11); }
static void fst80(uint32_t a,double v){
    uint64_t bits; memcpy(&bits,&v,8);
    uint16_t sign=bits>>63; int e=(bits>>52)&0x7FF; uint64_t frac=bits&0xFFFFFFFFFFFFFull;
    uint64_t mant; uint16_t exp;
    if(e==0 && frac==0){ mant=0; exp=0; }
    else if(e==0x7FF){ exp=0x7FFF; mant=0x8000000000000000ull|(frac<<11); }
    else if(e==0){ /* denormal */ int sh=0; uint64_t f=frac; while(!(f&(1ull<<52))){f<<=1;sh++;} exp=1-1023+16383-sh+0; mant=f<<11; }
    else { exp=e-1023+16383; mant=0x8000000000000000ull|(frac<<11); }
    *(uint64_t*)(gmem+a)=mant; *(uint16_t*)(gmem+a+8)=(sign<<15)|exp;
}
static double fld80(uint32_t a){
    uint64_t mant=*(uint64_t*)(gmem+a); uint16_t se=*(uint16_t*)(gmem+a+8);
    int sign=se>>15, exp=se&0x7FFF;
    if(exp==0&&mant==0) return sign?-0.0:0.0;
    double v=ldexp((double)mant, exp-16383-63);
    return sign?-v:v;
}

static void fpu(uint8_t op,uint32_t eip0){
    modrm();
    int r=reg_;
    if(mod_!=3){
        uint32_t a=ea_;
        switch(op){
        case 0xD8: case 0xDC: {
            double m = op==0xD8 ? (double)*(float*)(gmem+a) : *(double*)(gmem+a);
            if(a+7>=MEM_SIZE) gfault("fpu read",a);
            if(r==2||r==3){ fcmp_set(ST(0),m); if(r==3) fpop(); }
            else ST(0)=farith(r,ST(0),m);
            return; }
        case 0xDA: case 0xDE: {
            double m = op==0xDA ? (double)(int32_t)rd32(a) : (double)(int16_t)rd16(a);
            if(r==2||r==3){ fcmp_set(ST(0),m); if(r==3) fpop(); }
            else ST(0)=farith(r,ST(0),m);
            return; }
        case 0xD9:
            switch(r){
            case 0: { uint32_t u=rd32(a); float f; memcpy(&f,&u,4); fpush(f); return; }
            case 2: case 3: { float f=(float)ST(0); uint32_t u; memcpy(&u,&f,4); wr32(a,u); if(r==3)fpop(); return; }
            case 5: cpu.fcw=rd16(a); return;
            case 7: wr16(a,cpu.fcw); return;
            case 4: case 6: return; /* fldenv/fnstenv: ignore */
            }
            break;
        case 0xDB:
            switch(r){
            case 0: fpush((double)(int32_t)rd32(a)); return;
            case 2: case 3: { double v=fround(ST(0)); int32_t i = (v>=2147483648.0||v<-2147483648.0||isnan(v))?(int32_t)0x80000000:(int32_t)v; wr32(a,(uint32_t)i); if(r==3)fpop(); return; }
            case 5: rd8(a+9); fpush(fld80(a)); return;
            case 7: rd8(a+9); fst80(a,ST(0)); fpop(); return;
            }
            break;
        case 0xDD:
            switch(r){
            case 0: rd8(a+7); fpush(*(double*)(gmem+a)); return;
            case 2: case 3: rd8(a+7); *(double*)(gmem+a)=ST(0); if(r==3)fpop(); return;
            case 7: wr16(a,get_fsw()); return;
            case 4: case 6: return;
            }
            break;
        case 0xDF:
            switch(r){
            case 0: fpush((double)(int16_t)rd16(a)); return;
            case 2: case 3: { double v=fround(ST(0)); int16_t i=(v>=32768.0||v<-32768.0||isnan(v))?(int16_t)0x8000:(int16_t)v; wr16(a,(uint16_t)i); if(r==3)fpop(); return; }
            case 5: rd8(a+7); fpush((double)(int64_t)*(int64_t*)(gmem+a)); return;
            case 7: { rd8(a+7); double v=fround(ST(0)); int64_t i=(v>=9.2233720368547758e18||v<-9.2233720368547758e18||isnan(v))?INT64_MIN:(int64_t)v; *(int64_t*)(gmem+a)=i; fpop(); return; }
            }
            break;
        }
        unimpl("fpu mem",eip0,op,(mod_<<6)|(r<<3)|rm_);
    }
    int i=rm_;
    switch(op){
    case 0xD8:
        if(r==2||r==3){ fcmp_set(ST(0),ST(i)); if(r==3)fpop(); }
        else ST(0)=farith(r,ST(0),ST(i));
        return;
    case 0xDC: {
        int rr=r; if(rr==4)rr=5; else if(rr==5)rr=4; else if(rr==6)rr=7; else if(rr==7)rr=6;
        if(r>=2&&r<=3) unimpl("fpu DC",eip0,op,(mod_<<6)|(r<<3)|rm_);
        ST(i)=farith(rr,ST(i),ST(0)); /* farith(op,a,b): 4: a-b, 5: b-a */
        return; }
    case 0xDE: {
        if(r==3 && i==1){ fcmp_set(ST(0),ST(1)); fpop(); fpop(); return; }
        int rr=r; if(rr==4)rr=5; else if(rr==5)rr=4; else if(rr==6)rr=7; else if(rr==7)rr=6;
        if(r==2||r==3) unimpl("fpu DE",eip0,op,(mod_<<6)|(r<<3)|rm_);
        ST(i)=farith(rr,ST(i),ST(0)); fpop(); return; }
    case 0xD9:
        switch(r){
        case 0: fpush(ST(i)); return;
        case 1: { double t=ST(0); ST(0)=ST(i); ST(i)=t; return; }
        case 2: return; /* fnop */
        case 4:
            switch(i){
            case 0: ST(0)=-ST(0); return;
            case 1: ST(0)=fabs(ST(0)); return;
            case 4: fcmp_set(ST(0),0.0); return;
            case 5: { double v=ST(0); cpu.fsw_cc&=~((1<<8)|(1<<9)|(1<<10)|(1<<14)); if(signbit(v))cpu.fsw_cc|=1<<9;
                      if(isnan(v))cpu.fsw_cc|=1<<8; else if(isinf(v))cpu.fsw_cc|=(1<<8)|(1<<10); else if(v==0)cpu.fsw_cc|=1<<14; else cpu.fsw_cc|=1<<10; return; }
            }
            break;
        case 5:
            switch(i){
            case 0: fpush(1.0); return;
            case 1: fpush(log2(10.0)); return;
            case 2: fpush(M_LOG2E); return;
            case 3: fpush(M_PI); return;
            case 4: fpush(log10(2.0)); return;
            case 5: fpush(M_LN2); return;
            case 6: fpush(0.0); return;
            }
            break;
        case 6:
            switch(i){
            case 0: ST(0)=pow(2.0,ST(0))-1.0; return;
            case 1: { double x=ST(1),y=ST(0); fpop(); ST(0)=x*log2(y); return; }
            case 2: ST(0)=tan(ST(0)); fpush(1.0); cpu.fsw_cc&=~(1<<10); return;
            case 3: { double x=ST(0),y=ST(1); fpop(); ST(0)=atan2(y,x); return; }
            case 6: cpu.top=(cpu.top-1)&7; return;
            case 7: cpu.top=(cpu.top+1)&7; return;
            }
            break;
        case 7:
            switch(i){
            case 0: { double a=ST(0),b=ST(1); ST(0)=fmod(a,b); cpu.fsw_cc&=~(1<<10); return; }
            case 1: { double x=ST(1),y=ST(0); fpop(); ST(0)=x*log2(y+1.0); return; }
            case 2: ST(0)=sqrt(ST(0)); return;
            case 3: { double v=ST(0); ST(0)=sin(v); fpush(cos(v)); cpu.fsw_cc&=~(1<<10); return; }
            case 4: ST(0)=nearbyint(ST(0)); return;
            case 5: ST(0)=ST(0)*pow(2.0,trunc(ST(1))); return;
            case 6: ST(0)=sin(ST(0)); return;
            case 7: ST(0)=cos(ST(0)); return;
            }
            break;
        }
        break;
    case 0xDA:
        if(r==5&&i==1){ fcmp_set(ST(0),ST(1)); fpop(); fpop(); return; }
        if(r<4){ int c; switch(r){case 0:c=cpu.CF;break;case 1:c=cpu.ZF;break;case 2:c=cpu.CF||cpu.ZF;break;default:c=cpu.PF;} if(c) ST(0)=ST(i); return; }
        break;
    case 0xDB:
        if(r<4){ int c; switch(r){case 0:c=!cpu.CF;break;case 1:c=!cpu.ZF;break;case 2:c=!(cpu.CF||cpu.ZF);break;default:c=!cpu.PF;} if(c) ST(0)=ST(i); return; }
        if(r==4){ if(i==2){ cpu.fsw_cc=0; return; } if(i==3){ cpu.fcw=0x37F; cpu.fsw_cc=0; cpu.top=0; return; } }
        if(r==5){ fcomi_set(ST(0),ST(i)); return; }
        if(r==6){ fcomi_set(ST(0),ST(i)); return; }
        break;
    case 0xDD:
        switch(r){
        case 0: return; /* ffree */
        case 2: ST(i)=ST(0); return;
        case 3: ST(i)=ST(0); fpop(); return;
        case 4: fcmp_set(ST(0),ST(i)); return;
        case 5: fcmp_set(ST(0),ST(i)); fpop(); return;
        }
        break;
    case 0xDF:
        if(r==4&&i==0){ cpu.r[EAX]=(cpu.r[EAX]&0xFFFF0000u)|get_fsw(); return; }
        if(r==5){ fcomi_set(ST(0),ST(i)); fpop(); return; }
        if(r==6){ fcomi_set(ST(0),ST(i)); fpop(); return; }
        break;
    }
    unimpl("fpu reg",eip0,op,(mod_<<6)|(r<<3)|rm_);
}

/* ---------- 0F two-byte ---------- */
static void op0f(uint32_t eip0){
    uint8_t op=f8();
    int sz=opsz;
    if(op>=0x80&&op<=0x8F){ int32_t d=(int32_t)f32(); if(cond(op&15)) cpu.eip+=d; return; }
    if(op>=0x90&&op<=0x9F){ modrm(); set_rm(1,cond(op&15)); return; }
    if(op>=0x40&&op<=0x4F){ modrm(); uint32_t v=get_rm(sz); if(cond(op&15)) setreg(sz,reg_,v); return; }
    if(op>=0xC8&&op<=0xCF){ uint32_t v=cpu.r[op&7]; cpu.r[op&7]=(v>>24)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|(v<<24); return; }
    switch(op){
    case 0xB6: modrm(); setreg(sz,reg_,get_rm(1)); return;
    case 0xB7: modrm(); setreg(sz,reg_,get_rm(2)); return;
    case 0xBE: modrm(); setreg(sz,reg_,(uint32_t)(int32_t)(int8_t)get_rm(1)&szmask(sz)); return;
    case 0xBF: modrm(); setreg(sz,reg_,(uint32_t)(int32_t)(int16_t)get_rm(2)&szmask(sz)); return;
    case 0xAF: { modrm(); int64_t a= sz==4?(int32_t)getreg(sz,reg_):(int16_t)getreg(sz,reg_);
                 int64_t b= sz==4?(int32_t)get_rm(sz):(int16_t)get_rm(sz); int64_t p=a*b;
                 uint32_t res=(uint32_t)p&szmask(sz); setreg(sz,reg_,res);
                 int64_t t = sz==4?(int64_t)(int32_t)res:(int64_t)(int16_t)res; cpu.CF=cpu.OF=(t!=p); return; }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
        modrm(); uint32_t bit=getreg(sz,reg_); uint32_t v;
        if(mod_!=3){ ea_ += ((int32_t)bit>>(sz==4?5:4))*sz; bit&=sz*8-1; v=rdm(sz,ea_);} else { bit&=sz*8-1; v=getreg(sz,rm_);}
        cpu.CF=(v>>bit)&1;
        if(op==0xAB) v|=1u<<bit; else if(op==0xB3) v&=~(1u<<bit); else if(op==0xBB) v^=1u<<bit; else return;
        set_rm(sz,v); return; }
    case 0xBA: { modrm(); uint8_t b=f8()&(sz*8-1); uint32_t v=get_rm(sz); cpu.CF=(v>>b)&1;
                 switch(reg_){ case 5: v|=1u<<b; break; case 6: v&=~(1u<<b); break; case 7: v^=1u<<b; break; case 4: return; default: unimpl("0fba",eip0,op,reg_); }
                 set_rm(sz,v); return; }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
        modrm(); uint32_t cnt = (op&1)? cpu.r[ECX] : f8(); cnt&=31;
        uint32_t d=get_rm(sz), s=getreg(sz,reg_); int bits=sz*8; if(!cnt) return;
        uint64_t res;
        if(op<0xA8){ res=(((uint64_t)d<<bits)|s)<<cnt; cpu.CF=(res>>(2*bits))&1; res=(res>>bits)&szmask(sz); }
        else { res=(((uint64_t)s<<bits)|d)>>cnt; cpu.CF=(((uint64_t)((uint64_t)s<<bits|d))>>(cnt-1))&1; res&=szmask(sz); }
        cpu.OF=((d^(uint32_t)res)&szsign(sz))!=0; setszp((uint32_t)res,sz); set_rm(sz,(uint32_t)res); return; }
    case 0xBC: { modrm(); uint32_t v=get_rm(sz); if(!v){cpu.ZF=1;return;} cpu.ZF=0; int i=0; while(!((v>>i)&1))i++; setreg(sz,reg_,i); return; }
    case 0xBD: { modrm(); uint32_t v=get_rm(sz); if(!v){cpu.ZF=1;return;} cpu.ZF=0; int i=sz*8-1; while(!((v>>i)&1))i--; setreg(sz,reg_,i); return; }
    case 0xB0: case 0xB1: { int s=(op&1)?sz:1; modrm(); uint32_t d=get_rm(s); uint32_t a=getreg(s,EAX);
        alu(7,a,d,s); if(cpu.ZF) set_rm(s,getreg(s,reg_)); else setreg(s,EAX,d); return; }
    case 0xC0: case 0xC1: { int s=(op&1)?sz:1; modrm(); uint32_t d=get_rm(s), r2=getreg(s,reg_); uint32_t res=alu(0,d,r2,s); setreg(s,reg_,d); set_rm(s,res); return; }
    case 0xA2: { /* cpuid */ uint32_t leaf=cpu.r[EAX];
        if(leaf==0){ cpu.r[EAX]=1; cpu.r[EBX]=0x756e6547; cpu.r[EDX]=0x49656e69; cpu.r[ECX]=0x6c65746e; }
        else { cpu.r[EAX]=0x0000052C; cpu.r[EBX]=0; cpu.r[ECX]=0; cpu.r[EDX]=0x00000001; /* FPU only: force plain code paths */ }
        return; }
    case 0x31: { uint64_t t=cpu.icount*4; cpu.r[EAX]=(uint32_t)t; cpu.r[EDX]=(uint32_t)(t>>32); return; }
    case 0x0B: unimpl("ud2",eip0,0x0F,op);
    }
    unimpl("0f",eip0,0x0F,op);
}

/* ---------- string ops ---------- */
static void strop(uint8_t op,int rep){ /* rep: 0 none, 1 repe/rep, 2 repne */
    int sz=(op&1)?opsz:1; int d=cpu.DF?-sz:sz;
    uint32_t cnt = rep? cpu.r[ECX]:1;
    /* fast paths: rep movs / rep stos going forward, whole range inside guest memory (otherwise fall through to the exact element loop) */
    if(rep==1 && !cpu.DF && cnt>1 && ((op&0xFE)==0xA4 || (op&0xFE)==0xAA)){
        uint64_t len=(uint64_t)cnt*(uint64_t)sz, dst=cpu.r[EDI];
        if((op&0xFE)==0xA4){
            uint64_t src=cpu.r[ESI];
            if(src+len<=MEM_SIZE && dst+len<=MEM_SIZE && !(dst>src && dst<src+len)){   /* overlap with dst ahead of src must replicate element by element */
                memmove(gmem+dst,gmem+src,(size_t)len);
                cpu.r[ESI]=(uint32_t)(src+len); cpu.r[EDI]=(uint32_t)(dst+len); cpu.r[ECX]=0; return;
            }
        } else if(dst+len<=MEM_SIZE){
            uint32_t v=getreg(sz,EAX); uint8_t *p=gmem+dst;
            if(sz==1) memset(p,(int)(v&0xFF),(size_t)len);
            else if(sz==2){ uint16_t w=(uint16_t)v; for(uint32_t i=0;i<cnt;i++) ((uint16_t*)p)[i]=w; }
            else { for(uint32_t i=0;i<cnt;i++) ((uint32_t*)p)[i]=v; }
            cpu.r[EDI]=(uint32_t)(dst+len); cpu.r[ECX]=0; return;
        }
    }
    while(cnt){
        switch(op&0xFE){
        case 0xA4: wrm(sz,cpu.r[EDI],rdm(sz,cpu.r[ESI])); cpu.r[ESI]+=d; cpu.r[EDI]+=d; break;
        case 0xAA: wrm(sz,cpu.r[EDI],getreg(sz,EAX)); cpu.r[EDI]+=d; break;
        case 0xAC: setreg(sz,EAX,rdm(sz,cpu.r[ESI])); cpu.r[ESI]+=d; break;
        case 0xA6: alu(7,rdm(sz,cpu.r[ESI]),rdm(sz,cpu.r[EDI]),sz); cpu.r[ESI]+=d; cpu.r[EDI]+=d; break;
        case 0xAE: alu(7,getreg(sz,EAX),rdm(sz,cpu.r[EDI]),sz); cpu.r[EDI]+=d; break;
        }
        if(rep){ cpu.r[ECX]--; cnt=cpu.r[ECX];
            if((op&0xFE)==0xA6||(op&0xFE)==0xAE){ if(rep==1 && !cpu.ZF) break; if(rep==2 && cpu.ZF) break; } }
        else cnt=0;
    }
}

/* ---------- main step ---------- */
/* 1 for the prefix bytes step() understands (or ignores): 66 64 F3 F2 2E 36 3E 26 65 F0 67 */
static const uint8_t is_prefix[256]={ [0x66]=1,[0x64]=1,[0xF3]=1,[0xF2]=1,[0x2E]=1,[0x36]=1,[0x3E]=1,[0x26]=1,[0x65]=1,[0xF0]=1,[0x67]=1 };
static void step(void){
    uint32_t eip0=cpu.eip;
    opsz=4; segbase=0; int rep=0;
    uint8_t op;
    for(;;){
        op=f8();
        if(__builtin_expect(!is_prefix[op],1)) break;     /* common case: not a prefix byte (one table lookup instead of 11 compares) */
        if(op==0x66) opsz=2; else if(op==0x64) segbase=cpu.fs_base;
        else if(op==0xF3) rep=1; else if(op==0xF2) rep=2;
        /* 2E 36 3E 26 65 F0 67: ignored */
    }
    int sz=opsz;
    switch(op){
    case 0x00 ... 0x05: case 0x08 ... 0x0D: case 0x10 ... 0x15: case 0x18 ... 0x1D:
    case 0x20 ... 0x25: case 0x28 ... 0x2D: case 0x30 ... 0x35: case 0x38 ... 0x3D: {
        int aop=op>>3;
        switch(op&7){
        case 0: { modrm(); uint32_t r=alu(aop,get_rm(1),getreg(1,reg_),1); if(aop!=7)set_rm(1,r); return; }
        case 1: { modrm();
                  if(sz==4){ uint32_t b=cpu.r[reg_];
                      if(mod_==3){ uint32_t r=alu32(aop,cpu.r[rm_],b); if(aop!=7) cpu.r[rm_]=r; }
                      else { uint32_t r=alu32(aop,rd32(ea_),b); if(aop!=7) wr32(ea_,r); }
                      return; }
                  uint32_t r=alu(aop,get_rm(sz),getreg(sz,reg_),sz); if(aop!=7)set_rm(sz,r); return; }
        case 2: { modrm(); uint32_t r=alu(aop,getreg(1,reg_),get_rm(1),1); if(aop!=7)setreg(1,reg_,r); return; }
        case 3: { modrm();
                  if(sz==4){ uint32_t b= mod_==3? cpu.r[rm_] : rd32(ea_); uint32_t r=alu32(aop,cpu.r[reg_],b); if(aop!=7) cpu.r[reg_]=r; return; }
                  uint32_t r=alu(aop,getreg(sz,reg_),get_rm(sz),sz); if(aop!=7)setreg(sz,reg_,r); return; }
        case 4: { uint32_t r=alu(aop,getreg(1,EAX),f8(),1); if(aop!=7)setreg(1,EAX,r); return; }
        default:{ uint32_t imm= sz==4?f32():f16(); uint32_t r=ALU(aop,getreg(sz,EAX),imm,sz); if(aop!=7)setreg(sz,EAX,r); return; }
        }
        return; }
    case 0x40 ... 0x47: { if(sz==4){ uint32_t cf=cpu.CF; cpu.r[op&7]=alu32(0,cpu.r[op&7],1); cpu.CF=(uint8_t)cf; return; } setreg(sz,op&7,incdec(0,getreg(sz,op&7),sz)); return; }
    case 0x48 ... 0x4F: { if(sz==4){ uint32_t cf=cpu.CF; cpu.r[op&7]=alu32(5,cpu.r[op&7],1); cpu.CF=(uint8_t)cf; return; } setreg(sz,op&7,incdec(1,getreg(sz,op&7),sz)); return; }
    case 0x50 ... 0x57: { if(sz==4)push32(cpu.r[op&7]); else push16(cpu.r[op&7]); return; }
    case 0x58 ... 0x5F: { if(sz==4)cpu.r[op&7]=pop32(); else setreg(2,op&7,pop16()); return; }
    case 0x70 ... 0x7F: { int8_t d=(int8_t)f8(); if(cond(op&15)) cpu.eip+=d; return; }
    case 0x91 ... 0x97: { uint32_t t=getreg(sz,EAX); setreg(sz,EAX,getreg(sz,op&7)); setreg(sz,op&7,t); return; }
    case 0xB0 ... 0xB7: { setreg(1,op&7,f8()); return; }
    case 0xB8 ... 0xBF: { setreg(sz,op&7,sz==4?f32():f16()); return; }
    case 0xD8 ... 0xDF: { fpu(op,eip0); return; }
    case 0x0F: op0f(eip0); return;
    case 0x60: { uint32_t t=cpu.r[ESP]; for(int i=0;i<8;i++) push32(i==ESP?t:cpu.r[i]); return; }
    case 0x61: { for(int i=7;i>=0;i--){ uint32_t v=pop32(); if(i!=ESP)cpu.r[i]=v; } return; }
    case 0x68: push32(f32()); return;
    case 0x6A: push32((int32_t)(int8_t)f8()); return;
    case 0x69: case 0x6B: { modrm(); int32_t s=(int32_t)get_rm(sz); if(sz==2)s=(int16_t)s; int32_t imm = op==0x6B?(int8_t)f8():(sz==4?(int32_t)f32():(int16_t)f16());
                 int64_t p=(int64_t)s*imm; setreg(sz,reg_,(uint32_t)p&szmask(sz));
                 int64_t t= sz==4?(int64_t)(int32_t)p:(int64_t)(int16_t)p; cpu.CF=cpu.OF=(t!=p); return; }
    case 0x80: case 0x81: case 0x83: { int s=(op==0x80)?1:sz; modrm(); uint32_t imm= op==0x81?(s==4?f32():f16()): (op==0x83?(uint32_t)(int32_t)(int8_t)f8():f8());
                 if(s==4){ if(mod_==3){ uint32_t r=alu32(reg_,cpu.r[rm_],imm); if(reg_!=7) cpu.r[rm_]=r; } else { uint32_t r=alu32(reg_,rd32(ea_),imm); if(reg_!=7) wr32(ea_,r); } return; }
                 uint32_t r=alu(reg_,get_rm(s),imm,s); if(reg_!=7)set_rm(s,r); return; }
    case 0x84: { modrm(); alu(4,get_rm(1),getreg(1,reg_),1); return; }
    case 0x85: { modrm(); if(sz==4){ alu32(4,mod_==3?cpu.r[rm_]:rd32(ea_),cpu.r[reg_]); return; } alu(4,get_rm(sz),getreg(sz,reg_),sz); return; }
    case 0x86: { modrm(); uint32_t a=get_rm(1),b=getreg(1,reg_); set_rm(1,b); setreg(1,reg_,a); return; }
    case 0x87: { modrm(); uint32_t a=get_rm(sz),b=getreg(sz,reg_); set_rm(sz,b); setreg(sz,reg_,a); return; }
    case 0x88: modrm(); set_rm(1,getreg(1,reg_)); return;
    case 0x89: modrm(); if(sz==4){ if(mod_==3) cpu.r[rm_]=cpu.r[reg_]; else wr32(ea_,cpu.r[reg_]); return; } set_rm(sz,getreg(sz,reg_)); return;
    case 0x8A: modrm(); setreg(1,reg_,get_rm(1)); return;
    case 0x8B: modrm(); if(sz==4){ cpu.r[reg_]= mod_==3? cpu.r[rm_] : rd32(ea_); return; } setreg(sz,reg_,get_rm(sz)); return;
    case 0x8D: { uint32_t sb=segbase; segbase=0; modrm(); segbase=sb; setreg(sz,reg_,ea_); return; }
    case 0x8F: { modrm(); uint32_t v= sz==4?pop32():pop16(); set_rm(sz,v); return; }
    case 0x90: return;
    case 0x98: if(sz==4) cpu.r[EAX]=(uint32_t)(int32_t)(int16_t)cpu.r[EAX]; else setreg(2,EAX,(uint32_t)(int16_t)(int8_t)cpu.r[EAX]); return;
    case 0x99: if(sz==4) cpu.r[EDX]=((int32_t)cpu.r[EAX]<0)?0xFFFFFFFFu:0; else setreg(2,EDX,((int16_t)cpu.r[EAX]<0)?0xFFFF:0); return;
    case 0x9B: return;
    case 0x9C: push32(get_eflags()); return;
    case 0x9D: set_eflags(pop32()); return;
    case 0x9E: { uint32_t ah=(cpu.r[EAX]>>8)&0xFF; cpu.CF=ah&1; cpu.PF=(ah>>2)&1; cpu.AF=(ah>>4)&1; cpu.ZF=(ah>>6)&1; cpu.SF=(ah>>7)&1; return; }
    case 0x9F: { uint32_t f=2|cpu.CF|(cpu.PF<<2)|(cpu.AF<<4)|(cpu.ZF<<6)|(cpu.SF<<7); setreg(1,4,f); return; }
    case 0xA0: setreg(1,EAX,rd8(f32()+segbase)); return;
    case 0xA1: setreg(sz,EAX,rdm(sz,f32()+segbase)); return;
    case 0xA2: wr8(f32()+segbase,cpu.r[EAX]); return;
    case 0xA3: wrm(sz,f32()+segbase,getreg(sz,EAX)); return;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        strop(op,rep); return;
    case 0xA8: alu(4,getreg(1,EAX),f8(),1); return;
    case 0xA9: alu(4,getreg(sz,EAX),sz==4?f32():f16(),sz); return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int s=(op&1)?sz:1; modrm(); uint32_t cnt = op<0xD0? f8() : (op<0xD2?1:(cpu.r[ECX]&0xFF));
        set_rm(s,shiftop(reg_,get_rm(s),cnt,s)); return; }
    case 0xC2: { uint16_t n=f16(); uint32_t ra=pop32(); cpu.r[ESP]+=n; cpu.eip=ra; return; }
    case 0xC3: cpu.eip=pop32(); return;
    case 0xC6: { modrm(); set_rm(1,f8()); return; }
    case 0xC7: { modrm(); set_rm(sz,sz==4?f32():f16()); return; }
    case 0xC9: cpu.r[ESP]=cpu.r[EBP]; if(sz==4)cpu.r[EBP]=pop32(); else setreg(2,EBP,pop16()); return;
    case 0xCC: fprintf(stderr,"[int3] at 0x%08x\n",eip0); abort();
    case 0xCD: fprintf(stderr,"[int %02x] at 0x%08x\n",f8(),eip0); abort();
    case 0xD7: setreg(1,EAX,rd8(cpu.r[EBX]+(cpu.r[EAX]&0xFF))); return;
    case 0xE0: case 0xE1: case 0xE2: { int8_t d=(int8_t)f8(); cpu.r[ECX]--; int c=cpu.r[ECX]!=0;
        if(op==0xE0&&cpu.ZF)c=0; if(op==0xE1&&!cpu.ZF)c=0; if(c)cpu.eip+=d; return; }
    case 0xE3: { int8_t d=(int8_t)f8(); if(cpu.r[ECX]==0)cpu.eip+=d; return; }
    case 0xE8: { int32_t d=(int32_t)f32(); push32(cpu.eip); cpu.eip+=d; return; }
    case 0xE9: { int32_t d=(int32_t)f32(); cpu.eip+=d; return; }
    case 0xEB: { int8_t d=(int8_t)f8(); cpu.eip+=d; return; }
    case 0xF4: cpu.halted=1; return;
    case 0xF5: cpu.CF=!cpu.CF; return;
    case 0xF8: cpu.CF=0; return;
    case 0xF9: cpu.CF=1; return;
    case 0xFC: cpu.DF=0; return;
    case 0xFD: cpu.DF=1; return;
    case 0xF6: case 0xF7: {
        int s=(op&1)?sz:1; modrm(); uint32_t m=szmask(s);
        switch(reg_){
        case 0: case 1: { uint32_t imm= s==4?f32():(s==2?f16():f8()); alu(4,get_rm(s),imm,s); return; }
        case 2: set_rm(s,~get_rm(s)&m); return;
        case 3: { uint32_t v=get_rm(s); uint32_t r=alu(5,0,v,s); set_rm(s,r); return; }
        case 4: { uint64_t p; uint32_t a=getreg(s,EAX), b=get_rm(s);
            p=(uint64_t)a*b;
            if(s==1){ setreg(2,EAX,(uint32_t)p); cpu.CF=cpu.OF=((p>>8)!=0); }
            else if(s==2){ setreg(2,EAX,(uint32_t)p); setreg(2,EDX,(uint32_t)(p>>16)); cpu.CF=cpu.OF=((p>>16)!=0); }
            else { cpu.r[EAX]=(uint32_t)p; cpu.r[EDX]=(uint32_t)(p>>32); cpu.CF=cpu.OF=((p>>32)!=0); }
            return; }
        case 5: { int64_t a= s==4?(int32_t)cpu.r[EAX]:s==2?(int16_t)cpu.r[EAX]:(int8_t)cpu.r[EAX];
            int64_t b= s==4?(int32_t)get_rm(s):s==2?(int16_t)get_rm(s):(int8_t)get_rm(s); int64_t p=a*b;
            if(s==1){ setreg(2,EAX,(uint32_t)p); cpu.CF=cpu.OF=(p!=(int8_t)p); }
            else if(s==2){ setreg(2,EAX,(uint32_t)p); setreg(2,EDX,(uint32_t)(p>>16)); cpu.CF=cpu.OF=(p!=(int16_t)p); }
            else { cpu.r[EAX]=(uint32_t)p; cpu.r[EDX]=(uint32_t)(p>>32); cpu.CF=cpu.OF=(p!=(int32_t)p); }
            return; }
        case 6: { uint32_t b=get_rm(s); if(!b){fprintf(stderr,"[DIV0] at 0x%08x\n",eip0);abort();}
            if(s==1){ uint32_t a=cpu.r[EAX]&0xFFFF; setreg(1,EAX,a/b); setreg(1,4,a%b); }
            else if(s==2){ uint32_t a=((cpu.r[EDX]&0xFFFF)<<16)|(cpu.r[EAX]&0xFFFF); setreg(2,EAX,a/b); setreg(2,EDX,a%b); }
            else { uint64_t a=((uint64_t)cpu.r[EDX]<<32)|cpu.r[EAX]; cpu.r[EAX]=(uint32_t)(a/b); cpu.r[EDX]=(uint32_t)(a%b); }
            return; }
        default: { int64_t b= s==4?(int32_t)get_rm(s):s==2?(int16_t)get_rm(s):(int8_t)get_rm(s);
            if(!b){fprintf(stderr,"[IDIV0] at 0x%08x\n",eip0);abort();}
            if(s==1){ int32_t a=(int16_t)cpu.r[EAX]; setreg(1,EAX,(uint32_t)(a/b)); setreg(1,4,(uint32_t)(a%b)); }
            else if(s==2){ int32_t a=(int32_t)(((cpu.r[EDX]&0xFFFF)<<16)|(cpu.r[EAX]&0xFFFF)); setreg(2,EAX,(uint32_t)(a/b)); setreg(2,EDX,(uint32_t)(a%b)); }
            else { int64_t a=(int64_t)(((uint64_t)cpu.r[EDX]<<32)|cpu.r[EAX]); cpu.r[EAX]=(uint32_t)(a/b); cpu.r[EDX]=(uint32_t)(a%b); }
            return; }
        }
    }
    case 0xFE: { modrm(); uint32_t v=get_rm(1); if(reg_==0) set_rm(1,incdec(0,v,1)); else if(reg_==1) set_rm(1,incdec(1,v,1)); else unimpl("FE",eip0,op,reg_); return; }
    case 0xFF: {
        modrm();
        switch(reg_){
        case 0: set_rm(sz,incdec(0,get_rm(sz),sz)); return;
        case 1: set_rm(sz,incdec(1,get_rm(sz),sz)); return;
        case 2: { uint32_t t=get_rm(4); push32(cpu.eip); cpu.eip=t; return; }
        case 4: cpu.eip=get_rm(4); return;
        case 6: push32(get_rm(sz)); return;
        }
        unimpl("FF",eip0,op,reg_); }
    }
    unimpl("op",eip0,op,0);
}


/* ============ fast path ============
   Handlers for the most common instructions WITHOUT prefixes (so 32-bit operands, no segment override).
   Each one produces exactly what step() would for the same bytes; anything unusual is handed back to step().
   They are small leaf functions, so the CPU doesn't pay step()'s huge prologue/epilogue for every guest instruction. */
typedef void (*fastfn)(uint32_t op);
static inline __attribute__((always_inline)) uint32_t f_ea(int mod,int rm){
    uint32_t ea;
    if(rm==4){
        uint8_t sib=f8(); int sc=sib>>6, idx=(sib>>3)&7, base=sib&7;
        if(base==5 && mod==0) ea=f32(); else ea=cpu.r[base];
        if(idx!=4) ea += cpu.r[idx]<<sc;
    } else if(rm==5 && mod==0) ea=f32();
    else ea=cpu.r[rm];
    if(mod==1) ea += (int32_t)(int8_t)f8();
    else if(mod==2) ea += f32();
    return ea;
}
#define FM_DECODE() uint8_t _m=f8(); const int mod=_m>>6, reg=(_m>>3)&7, rm=_m&7; uint32_t ea=0; if(mod!=3) ea=f_ea(mod,rm)
#define RM32_READ()   (mod==3? cpu.r[rm] : rd32(ea))
#define RM32_WRITE(v) do{ if(mod==3) cpu.r[rm]=(v); else wr32(ea,(v)); }while(0)
#define RM8_READ()    (mod==3? getreg(1,rm) : (uint32_t)rd8(ea))
#define RM8_WRITE(v)  do{ if(mod==3) setreg(1,rm,(v)); else wr8(ea,(uint8_t)(v)); }while(0)
#define EIP0 (cpu.eip-1)
#define FALLBACK() do{ cpu.eip=EIP0; step(); return; }while(0)

#define DEF_ALU(NAME,AOP) \
static void NAME##_rm_r(uint32_t op){ (void)op; FM_DECODE(); uint32_t r=alu32(AOP,RM32_READ(),cpu.r[reg]); if(AOP!=7) RM32_WRITE(r); } \
static void NAME##_r_rm(uint32_t op){ (void)op; FM_DECODE(); uint32_t b=RM32_READ(); uint32_t r=alu32(AOP,cpu.r[reg],b); if(AOP!=7) cpu.r[reg]=r; } \
static void NAME##_eax_i(uint32_t op){ (void)op; uint32_t imm=f32(); uint32_t r=alu32(AOP,cpu.r[EAX],imm); if(AOP!=7) cpu.r[EAX]=r; }
DEF_ALU(h_add,0) DEF_ALU(h_or,1) DEF_ALU(h_adc,2) DEF_ALU(h_sbb,3) DEF_ALU(h_and,4) DEF_ALU(h_sub,5) DEF_ALU(h_xor,6) DEF_ALU(h_cmp,7)

static void h_alu8_rm_r(uint32_t op){ int aop=op>>3; FM_DECODE(); uint32_t r=alu(aop,RM8_READ(),getreg(1,reg),1); if(aop!=7) RM8_WRITE(r); }
static void h_alu8_r_rm(uint32_t op){ int aop=op>>3; FM_DECODE(); uint32_t r=alu(aop,getreg(1,reg),RM8_READ(),1); if(aop!=7) setreg(1,reg,r); }
static void h_alu8_al_i(uint32_t op){ int aop=op>>3; uint32_t imm=f8(); uint32_t r=alu(aop,getreg(1,EAX),imm,1); if(aop!=7) setreg(1,EAX,r); }
static void h_80(uint32_t op){ (void)op; FM_DECODE(); uint32_t imm=f8(); uint32_t r=alu(reg,RM8_READ(),imm,1); if(reg!=7) RM8_WRITE(r); }
static void h_81(uint32_t op){ (void)op; FM_DECODE(); uint32_t imm=f32(); uint32_t r=alu32(reg,RM32_READ(),imm); if(reg!=7) RM32_WRITE(r); }
static void h_83(uint32_t op){ (void)op; FM_DECODE(); uint32_t imm=(uint32_t)(int32_t)(int8_t)f8(); uint32_t r=alu32(reg,RM32_READ(),imm); if(reg!=7) RM32_WRITE(r); }
static void h_84(uint32_t op){ (void)op; FM_DECODE(); alu(4,RM8_READ(),getreg(1,reg),1); }
static void h_85(uint32_t op){ (void)op; FM_DECODE(); alu32(4,RM32_READ(),cpu.r[reg]); }
static void h_A8(uint32_t op){ (void)op; alu(4,getreg(1,EAX),f8(),1); }
static void h_A9(uint32_t op){ (void)op; alu32(4,cpu.r[EAX],f32()); }
static void h_88(uint32_t op){ (void)op; FM_DECODE(); RM8_WRITE(getreg(1,reg)); }
static void h_89(uint32_t op){ (void)op; FM_DECODE(); RM32_WRITE(cpu.r[reg]); }
static void h_8A(uint32_t op){ (void)op; FM_DECODE(); setreg(1,reg,RM8_READ()); }
static void h_8B(uint32_t op){ (void)op; FM_DECODE(); cpu.r[reg]=RM32_READ(); }
static void h_8D(uint32_t op){ (void)op; if((gmem[cpu.eip]>>6)==3) FALLBACK(); FM_DECODE(); cpu.r[reg]=ea; }
static void h_C6(uint32_t op){ (void)op; FM_DECODE(); RM8_WRITE(f8()); }
static void h_C7(uint32_t op){ (void)op; FM_DECODE(); uint32_t imm=f32(); RM32_WRITE(imm); }
static void h_movri(uint32_t op){ cpu.r[op&7]=f32(); }
static void h_movri8(uint32_t op){ setreg(1,op&7,f8()); }
static void h_push(uint32_t op){ push32(cpu.r[op&7]); }
static void h_pop(uint32_t op){ cpu.r[op&7]=pop32(); }
static void h_push_i32(uint32_t op){ (void)op; push32(f32()); }
static void h_push_i8(uint32_t op){ (void)op; push32((uint32_t)(int32_t)(int8_t)f8()); }
static void h_inc(uint32_t op){ uint32_t cf=cpu.CF; cpu.r[op&7]=alu32(0,cpu.r[op&7],1); cpu.CF=(uint8_t)cf; }
static void h_dec(uint32_t op){ uint32_t cf=cpu.CF; cpu.r[op&7]=alu32(5,cpu.r[op&7],1); cpu.CF=(uint8_t)cf; }
static void h_jcc(uint32_t op){ int8_t d=(int8_t)f8(); if(cond(op&15)) cpu.eip+=d; }
static void h_jmp8(uint32_t op){ (void)op; int8_t d=(int8_t)f8(); cpu.eip+=d; }
static void h_jmp32(uint32_t op){ (void)op; int32_t d=(int32_t)f32(); cpu.eip+=d; }
static void h_call(uint32_t op){ (void)op; int32_t d=(int32_t)f32(); push32(cpu.eip); cpu.eip+=d; }
static void h_ret(uint32_t op){ (void)op; cpu.eip=pop32(); }
static void h_leave(uint32_t op){ (void)op; cpu.r[ESP]=cpu.r[EBP]; cpu.r[EBP]=pop32(); }
static void h_nop(uint32_t op){ (void)op; }
static void h_cwde(uint32_t op){ (void)op; cpu.r[EAX]=(uint32_t)(int32_t)(int16_t)cpu.r[EAX]; }
static void h_cdq(uint32_t op){ (void)op; cpu.r[EDX]=((int32_t)cpu.r[EAX]<0)?0xFFFFFFFFu:0; }
static void h_shift(uint32_t op){ FM_DECODE(); uint32_t cnt = op==0xC1? f8() : (op==0xD1? 1 : (cpu.r[ECX]&0xFF)); uint32_t v=RM32_READ(); RM32_WRITE(shiftop(reg,v,cnt,4)); }
static void h_FF(uint32_t op){ (void)op; uint32_t eip0=EIP0; FM_DECODE();
    switch(reg){
    case 0: { uint32_t v=RM32_READ(); uint32_t cf=cpu.CF; uint32_t r=alu32(0,v,1); cpu.CF=(uint8_t)cf; RM32_WRITE(r); return; }
    case 1: { uint32_t v=RM32_READ(); uint32_t cf=cpu.CF; uint32_t r=alu32(5,v,1); cpu.CF=(uint8_t)cf; RM32_WRITE(r); return; }
    case 2: { uint32_t t=RM32_READ(); push32(cpu.eip); cpu.eip=t; return; }
    case 4: cpu.eip=RM32_READ(); return;
    case 6: push32(RM32_READ()); return;
    }
    cpu.eip=eip0; step(); }
static void h_fpu(uint32_t op){ uint32_t eip0=EIP0; opsz=4; segbase=0; fpu((uint8_t)op,eip0); }
static void h_0F(uint32_t op){ (void)op;
    uint8_t op2=gmem[cpu.eip];
    switch(op2){
    case 0x80 ... 0x8F: { cpu.eip++; int32_t d=(int32_t)f32(); if(cond(op2&15)) cpu.eip+=d; return; }
    case 0xB6: { cpu.eip++; FM_DECODE(); cpu.r[reg]=RM8_READ(); return; }
    case 0xB7: { cpu.eip++; FM_DECODE(); cpu.r[reg]= mod==3? (cpu.r[rm]&0xFFFFu) : (uint32_t)rd16(ea); return; }
    case 0xBE: { cpu.eip++; FM_DECODE(); cpu.r[reg]=(uint32_t)(int32_t)(int8_t)RM8_READ(); return; }
    case 0xBF: { cpu.eip++; FM_DECODE(); cpu.r[reg]=(uint32_t)(int32_t)(int16_t)(mod==3? (cpu.r[rm]&0xFFFFu) : (uint32_t)rd16(ea)); return; }
    case 0xAF: { cpu.eip++; FM_DECODE(); int64_t a=(int32_t)cpu.r[reg]; int64_t b=(int32_t)RM32_READ(); int64_t p=a*b;
                 uint32_t res=(uint32_t)p; cpu.r[reg]=res; int64_t t=(int64_t)(int32_t)res; cpu.CF=cpu.OF=(t!=p); return; }
    case 0x90 ... 0x9F: { cpu.eip++; FM_DECODE(); RM8_WRITE(cond(op2&15)); return; }
    case 0x40 ... 0x4F: { cpu.eip++; FM_DECODE(); uint32_t v=RM32_READ(); if(cond(op2&15)) cpu.r[reg]=v; return; }
    }
    cpu.eip=EIP0; step(); }

static const fastfn fast_tab[256]={
    [0x00]=h_alu8_rm_r,[0x02]=h_alu8_r_rm,[0x04]=h_alu8_al_i, [0x08]=h_alu8_rm_r,[0x0A]=h_alu8_r_rm,[0x0C]=h_alu8_al_i,
    [0x10]=h_alu8_rm_r,[0x12]=h_alu8_r_rm,[0x14]=h_alu8_al_i, [0x18]=h_alu8_rm_r,[0x1A]=h_alu8_r_rm,[0x1C]=h_alu8_al_i,
    [0x20]=h_alu8_rm_r,[0x22]=h_alu8_r_rm,[0x24]=h_alu8_al_i, [0x28]=h_alu8_rm_r,[0x2A]=h_alu8_r_rm,[0x2C]=h_alu8_al_i,
    [0x30]=h_alu8_rm_r,[0x32]=h_alu8_r_rm,[0x34]=h_alu8_al_i, [0x38]=h_alu8_rm_r,[0x3A]=h_alu8_r_rm,[0x3C]=h_alu8_al_i,
    [0x01]=h_add_rm_r,[0x03]=h_add_r_rm,[0x05]=h_add_eax_i, [0x09]=h_or_rm_r,[0x0B]=h_or_r_rm,[0x0D]=h_or_eax_i,
    [0x11]=h_adc_rm_r,[0x13]=h_adc_r_rm,[0x15]=h_adc_eax_i, [0x19]=h_sbb_rm_r,[0x1B]=h_sbb_r_rm,[0x1D]=h_sbb_eax_i,
    [0x21]=h_and_rm_r,[0x23]=h_and_r_rm,[0x25]=h_and_eax_i, [0x29]=h_sub_rm_r,[0x2B]=h_sub_r_rm,[0x2D]=h_sub_eax_i,
    [0x31]=h_xor_rm_r,[0x33]=h_xor_r_rm,[0x35]=h_xor_eax_i, [0x39]=h_cmp_rm_r,[0x3B]=h_cmp_r_rm,[0x3D]=h_cmp_eax_i,
    [0x40]=h_inc,[0x41]=h_inc,[0x42]=h_inc,[0x43]=h_inc,[0x44]=h_inc,[0x45]=h_inc,[0x46]=h_inc,[0x47]=h_inc,
    [0x48]=h_dec,[0x49]=h_dec,[0x4A]=h_dec,[0x4B]=h_dec,[0x4C]=h_dec,[0x4D]=h_dec,[0x4E]=h_dec,[0x4F]=h_dec,
    [0x50]=h_push,[0x51]=h_push,[0x52]=h_push,[0x53]=h_push,[0x54]=h_push,[0x55]=h_push,[0x56]=h_push,[0x57]=h_push,
    [0x58]=h_pop,[0x59]=h_pop,[0x5A]=h_pop,[0x5B]=h_pop,[0x5C]=h_pop,[0x5D]=h_pop,[0x5E]=h_pop,[0x5F]=h_pop,
    [0x68]=h_push_i32,[0x6A]=h_push_i8,
    [0x70]=h_jcc,[0x71]=h_jcc,[0x72]=h_jcc,[0x73]=h_jcc,[0x74]=h_jcc,[0x75]=h_jcc,[0x76]=h_jcc,[0x77]=h_jcc,
    [0x78]=h_jcc,[0x79]=h_jcc,[0x7A]=h_jcc,[0x7B]=h_jcc,[0x7C]=h_jcc,[0x7D]=h_jcc,[0x7E]=h_jcc,[0x7F]=h_jcc,
    [0x80]=h_80,[0x81]=h_81,[0x83]=h_83,[0x84]=h_84,[0x85]=h_85,
    [0x88]=h_88,[0x89]=h_89,[0x8A]=h_8A,[0x8B]=h_8B,[0x8D]=h_8D,
    [0x90]=h_nop,[0x98]=h_cwde,[0x99]=h_cdq,
    [0xA8]=h_A8,[0xA9]=h_A9,
    [0xB0]=h_movri8,[0xB1]=h_movri8,[0xB2]=h_movri8,[0xB3]=h_movri8,[0xB4]=h_movri8,[0xB5]=h_movri8,[0xB6]=h_movri8,[0xB7]=h_movri8,
    [0xB8]=h_movri,[0xB9]=h_movri,[0xBA]=h_movri,[0xBB]=h_movri,[0xBC]=h_movri,[0xBD]=h_movri,[0xBE]=h_movri,[0xBF]=h_movri,
    [0xC1]=h_shift,[0xD1]=h_shift,[0xD3]=h_shift,
    [0xC3]=h_ret,[0xC6]=h_C6,[0xC7]=h_C7,[0xC9]=h_leave,
    [0xD8]=h_fpu,[0xD9]=h_fpu,[0xDA]=h_fpu,[0xDB]=h_fpu,[0xDC]=h_fpu,[0xDD]=h_fpu,[0xDE]=h_fpu,[0xDF]=h_fpu,
    [0xE8]=h_call,[0xE9]=h_jmp32,[0xEB]=h_jmp8,
    [0xFF]=h_FF, [0x0F]=h_0F,
};


/* Guest hot-spot sampler: every 4096 guest instructions, note which 64-byte block of code EIP is in.
   cpu_prof_report() turns the last second of samples into "which guest loops are hot" (address + code bytes) for the perf log. */
#define PROF_BLOCKS 8192
static uint32_t prof_hist[PROF_BLOCKS]; static uint64_t prof_next=4096;
int cpu_prof_report(char *out,int n){
    static uint32_t prev[PROF_BLOCKS]; uint32_t d[PROF_BLOCKS]; uint64_t tot=0; int len=0;
    if(!out||n<=0) return 0; out[0]=0;
    for(int i=0;i<PROF_BLOCKS;i++){ d[i]=prof_hist[i]-prev[i]; prev[i]=prof_hist[i]; tot+=d[i]; }
    if(tot<8) return 0;
    for(int k=0;k<3;k++){
        int best=-1; uint32_t bv=0; for(int i=0;i<PROF_BLOCKS;i++) if(d[i]>bv){ bv=d[i]; best=i; }
        if(best<0 || (uint64_t)bv*20<tot) break;                       /* ignore blocks under 5% */
        uint32_t a=g_code_lo+((uint32_t)best<<6), from=a>=g_code_lo+32?a-32:a;
        if(len<n) len+=snprintf(out+len,n-len,"\n핫스팟 %08x %.0f%% 코드@%08x:",a,100.0*bv/tot,from);
        for(int j=0;j<128 && len<n-3;j++) len+=snprintf(out+len,n-len,"%02x",gmem[from+j]);
        d[best]=0;
    }
    return len<n?len:n-1;
}

void cpu_run(uint32_t stop_eip){
    while(!cpu.halted){
        if(cpu.eip==stop_eip) return;
        if(sched_multi && g_nest==0 && cpu.icount>=next_slice){ next_slice=cpu.icount+131072; sched_switch(); }
        if(cpu.eip==THREAD_EXIT_MAGIC){ sched_thread_exit(); continue; }
        if(cpu.eip>=HLE_BASE){ hle_dispatch((cpu.eip-HLE_BASE)>>2); if(g_switch&&g_nest==0){ g_switch=0; sched_switch(); } continue; }
        if(cpu.eip<g_code_lo||cpu.eip>=g_code_hi){ fprintf(stderr,"[BAD EIP] jumped to %08x\n",cpu.eip); fprintf(stderr,"  last EIPs:"); for(int i=40;i>0;i--) fprintf(stderr," %08x",ring[(ringp-i)&255]); fprintf(stderr,"\n  regs EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",cpu.r[0],cpu.r[1],cpu.r[2],cpu.r[3],cpu.r[4],cpu.r[5],cpu.r[6],cpu.r[7]); abort(); }
        {   /* hot loop: stay here while EIP is plain guest code (HLE / thread-exit addresses are outside [g_code_lo,g_code_hi)) */
            uint64_t lim=(sched_multi && g_nest==0)? next_slice : ~0ull;
            if(lim>prof_next) lim=prof_next;
            const uint32_t span=g_code_hi-g_code_lo;
            do {
        #ifndef RBO_LIBRARY
                ring[ringp++&255]=cpu.eip;   /* debug trail for crash dumps; skipped in the embedded (iOS) build */
        #endif
                { fastfn f=fast_tab[gmem[cpu.eip]]; if(f){ uint32_t op=gmem[cpu.eip++]; f(op); } else step(); }
                cpu.icount++;
            } while(!cpu.halted && (uint32_t)(cpu.eip-g_code_lo)<span && cpu.eip!=stop_eip && cpu.icount<lim);
            if(cpu.icount>=prof_next){ prof_next=cpu.icount+4096; uint32_t b=(cpu.eip-g_code_lo)>>6; if(b<PROF_BLOCKS) prof_hist[b]++; }
        }
    }
}
