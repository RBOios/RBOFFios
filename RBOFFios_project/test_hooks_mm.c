/* test_hooks_mm.c - bit-exact A/B test of the mm4 (0x412b40) / mm3 (0x412f00) hooks, no exe needed:
 * the function bodies are copied into guest memory from hooks_mm.h's byte arrays?  (they are static there) -> include hooks_mm.h's data via hooks.c is not possible,
 * so the bodies are taken from code_412000.bin (dumped from the exe / a save state) given on the command line.
 *   make test_hooks_mm && ./test_hooks_mm code_412000.bin [iterations]       (the .bin holds guest memory 0x412000..0x414000) */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
void rbo_install_hooks_mask(int mask); void rbo_uninstall_hooks(void);
static uint64_t rs=88172645463325252ull;
static uint32_t rnd(void){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return (uint32_t)(rs>>11); }
static uint32_t rfloat(void){
    static const uint32_t sp[]={0x00000000,0x80000000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc00000,0x7f800001,0xffc12345,0x00000001,0x007fffff,0x80000042,0x7f7fffff,0xff7fffff,0x00800000,0x4b800000};
    switch(rnd()%6){ case 0: return sp[rnd()%16]; case 1: return rnd()<<9^rnd();   /* any bit pattern */
    case 2: { float f=(float)((int)(rnd()%2001)-1000)/16.0f; uint32_t u; memcpy(&u,&f,4); return u; }
    default:{ float f=((float)(rnd()%2000000)-1000000.0f)/1e3f*(rnd()%3?1.0f:1e-6f); uint32_t u; memcpy(&u,&f,4); return u; } }
}
int main(int argc,char **argv){
    if(argc<2){ fprintf(stderr,"usage: test_hooks_mm code_412000.bin [iters]\n"); return 1; }
    int iters=argc>2?atoi(argv[2]):20000;
    gmem=mmap(NULL,MEM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    cpu_init(); hle_init(); cpu.r[ESP]=0x1FFFE000u;
    FILE *f=fopen(argv[1],"rb"); if(!f){ perror("bin"); return 1; } if(fread(gmem+0x412000,1,0x2000,f)!=0x2000){ fprintf(stderr,"short bin\n"); return 1; } fclose(f);
    const uint32_t fn[2]={0x412b40,0x412f00}, base=0x01000000; const uint32_t SZ=0x400;
    uint8_t orig5[2][5], hook5[2][5];
    for(int w=0;w<2;w++) memcpy(orig5[w],gmem+fn[w],5);
    rbo_install_hooks_mask(0x18);
    for(int w=0;w<2;w++) memcpy(hook5[w],gmem+fn[w],5);
    if(!memcmp(orig5[0],hook5[0],5)||!memcmp(orig5[1],hook5[1],5)){ fprintf(stderr,"hooks were not installed\n"); return 1; }
#define SET_MODE(m) do{ for(int q=0;q<2;q++) memcpy(gmem+fn[q],(m)==0?hook5[q]:orig5[q],5); }while(0)
    static uint8_t snap[0x400], resN[0x400], resO[0x400]; int bad=0; long cases=0, nan_only=0;
    for(int it=0;it<iters;it++) for(int w=0;w<2;w++){
        for(uint32_t i=0;i<SZ;i+=4) wr32(base+i,rfloat());
        uint32_t A=base+4*(rnd()%64), B=base+4*(rnd()%64), D=base+4*(rnd()%64);
        switch(rnd()%6){ case 0: D=A; break; case 1: D=B; break; case 2: B=A; break; case 3: D=A; B=A; break; case 4: D=A+4*(rnd()%16); break; default: break; }
        if(A+0x40>base+SZ) A=base; if(B+0x40>base+SZ) B=base; if(D+0x40>base+SZ) D=base;
        memcpy(snap,gmem+base,SZ);
        uint32_t args[3]={D,A,B}; uint32_t regs[2][8];
        for(int mode=0;mode<2;mode++){                                 /* 0 = hooks on, 1 = original interpreter */
            memcpy(gmem+base,snap,SZ);
            cpu.r[EBX]=0x11111111; cpu.r[ESI]=0x22222222; cpu.r[EDI]=0x33333333; cpu.r[EBP]=0x44444444; cpu.r[ECX]=0xAAAA0001; cpu.r[EDX]=0xBBBB0002;
            SET_MODE(mode);
            uint32_t e=guest_call(fn[w],3,args);
            regs[mode][0]=e; regs[mode][1]=cpu.r[ECX]; regs[mode][2]=cpu.r[EDX]; regs[mode][3]=cpu.r[ESP]; regs[mode][4]=cpu.r[EBX]; regs[mode][5]=cpu.r[ESI]; regs[mode][6]=cpu.r[EDI]; regs[mode][7]=cpu.r[EBP];
            memcpy(mode==0?resN:resO,gmem+base,SZ);
        }
        cases++;
        int memdiff=0, nandiff=0;                                      /* NaN payloads may differ (host compilers may swap the operands of a*b); everything else must match */
        for(uint32_t i=0;i<SZ;i+=4){ uint32_t x,y; memcpy(&x,resN+i,4); memcpy(&y,resO+i,4); if(x==y) continue;
            if((x&0x7f800000)==0x7f800000&&(x&0x7fffff)&&(y&0x7f800000)==0x7f800000&&(y&0x7fffff)) nandiff++; else memdiff++; }
        nan_only+=(nandiff&&!memdiff); 
        if(memdiff||memcmp(regs[0],regs[1],sizeof regs[0])){ if(bad++<10) fprintf(stderr,"MISMATCH fn=%x D=%x A=%x B=%x memdiff=%d regdiff=%d\n",fn[w],D,A,B,memdiff!=0,memcmp(regs[0],regs[1],sizeof regs[0])!=0); }
    }
    printf("%ld cases, %d real mismatches, %ld cases differing only in NaN payload bits\n",cases,bad,nan_only);
    /* speed: 200000 calls each way */
    for(uint32_t i=0;i<SZ;i+=4) wr32(base+i,0x3f800000+(i<<8)); uint32_t args[3]={base+0x100,base,base+0x40};
    for(int mode=0;mode<2;mode++){ SET_MODE(mode);
        clock_t t0=clock(); for(int i=0;i<200000;i++) guest_call(0x412b40,3,args); double s=(double)(clock()-t0)/CLOCKS_PER_SEC;
        printf("%s: %.0f ns per mm4 call\n",mode==0?"native hook ":"interpreter ",s*1e9/200000); }
    return bad?1:0; }
