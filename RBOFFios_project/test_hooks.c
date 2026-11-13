/* test_hooks.c - A/B test: run the REAL guest code of RBO_Ex3.exe in the interpreter, then again with the native
 * hooks installed, on random inputs, and compare surface memory / return value / callee-saved registers / ESP.
 *   make test_hooks && ./test_hooks /path/to/RBO_Ex3.exe [iterations]
 */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

uint32_t pe_load(const char *path);
void rbo_install_hooks_mask(int mask);
void rbo_uninstall_hooks(void);

/* ---- fake DirectDraw surface: vtable +0x64 Lock, +0x80 Unlock ---- */
static struct { uint32_t pix, pitch, bpp, rm, gm, bm, am; } fs;
static int n_lock, n_unlock;
static void t_lock(void){ uint32_t d=A(2); n_lock++;
    wr32(d+0x10,fs.pitch); wr32(d+0x24,fs.pix); wr32(d+0x54,fs.bpp); wr32(d+0x58,fs.rm); wr32(d+0x5c,fs.gm); wr32(d+0x60,fs.bm); wr32(d+0x64,fs.am); RET(0); }
static void t_unlock(void){ n_unlock++; RET(0); }

static uint64_t rs=88172645463325252ull;
static uint32_t rnd(void){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return (uint32_t)(rs>>11); }

typedef struct { uint32_t r[8]; uint32_t ret; uint32_t esp; } Res;
static uint32_t run(uint32_t fn,int n,const uint32_t *args, uint32_t *regs){
    cpu.r[EBX]=0x11111111; cpu.r[ESI]=0x22222222; cpu.r[EDI]=0x33333333; cpu.r[EBP]=0x44444444;
    uint32_t esp0=cpu.r[ESP];
    uint32_t r=guest_call(fn,n,args);
    regs[0]=cpu.r[EBX]; regs[1]=cpu.r[ESI]; regs[2]=cpu.r[EDI]; regs[3]=cpu.r[EBP]; regs[4]=cpu.r[ESP]-esp0;
    return r;
}

static int fails=0, cases=0;
#define MAXSURF (512*512*4)
static uint8_t *snapA;
static void fmt(int k){
    static const struct{ uint32_t bpp,rm,gm,bm,am; } F[]={
        {16,0xF800,0x07E0,0x001F,0},{16,0x0F00,0x00F0,0x000F,0xF000},{16,0x7C00,0x03E0,0x001F,0x8000},
        {32,0x00FF0000,0x0000FF00,0x000000FF,0xFF000000},{32,0x000000FF,0x0000FF00,0x00FF0000,0xFF000000}};
    fs.bpp=F[k].bpp; fs.rm=F[k].rm; fs.gm=F[k].gm; fs.bm=F[k].bm; fs.am=F[k].am;
}

int main(int argc,char **argv){
    if(argc<2){ fprintf(stderr,"usage: test_hooks RBO_Ex3.exe [iters]\n"); return 1; }
    int iters = argc>2?atoi(argv[2]):300;
    gmem=mmap(NULL,MEM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    cpu_init(); hle_init(); pe_load(argv[1]);
    cpu.r[ESP]=0x1FFFE000u;
    int iL=hle_register("t","Lock",5,t_lock), iU=hle_register("t","Unlock",2,t_unlock);
    uint32_t vt=ga_alloc(0x100,1), obj=ga_alloc(16,1); wr32(obj,vt); wr32(vt+0x64,hle_thunk(iL)); wr32(vt+0x80,hle_thunk(iU));
    uint32_t pitchmax=512*4;
    fs.pix=ga_alloc(MAXSURF+64,1);
    snapA=malloc(MAXSURF+64);
    uint32_t srcbuf=ga_alloc(1<<20,1), palbuf=ga_alloc(1100,1);

    const char *nm[2]={"conv 0x414d10","rect 0x415060"};
    for(int which=0;which<2;which++){
        int per_fail_before=fails, per_cases=0;
        for(int it=0;it<iters;it++){
            int f=rnd()%5; fmt(f);
            uint32_t sw=8+rnd()%120, sh=8+rnd()%100;
            fs.pitch = fs.bpp==32 ? (sw*4+ (rnd()%3)*16) : (sw*2+(rnd()%3)*8);
            if(fs.pitch<sw*(fs.bpp/8)) fs.pitch=sw*(fs.bpp/8);
            uint32_t dx=rnd()%8, dy=rnd()%8, w, h;
            uint32_t mode;
            uint32_t args[11];
            for(uint32_t i=0;i<(1<<20);i+=4) wr32(srcbuf+i,rnd());
            for(uint32_t i=0;i<1024;i+=4) wr32(palbuf+i,rnd());
            if(rnd()%4==0) { wr8(palbuf+0,0); }
            uint32_t sx=0,sy=0,stride=0;
            if(which==0){
                mode=rnd()%4; w=1+rnd()%(sw-dx); h=1+rnd()%(sh-dy);
                if(rnd()%10==0) mode=4+rnd()%2;                 /* unhandled -> fallback path */
                args[0]=obj; args[1]=srcbuf; args[2]=dx; args[3]=dy; args[4]=w; args[5]=h; args[6]=palbuf; args[7]=mode;
            } else {
                mode=rnd()%3; w=1+rnd()%(sw-dx); h=1+rnd()%(sh-dy);
                stride=w+rnd()%20; sx=rnd()%5; sy=rnd()%5; if(rnd()%10==0) mode=3+rnd()%2;
                args[0]=obj; args[1]=srcbuf; args[2]=sx; args[3]=sy; args[4]=stride; args[5]=rnd(); args[6]=dx; args[7]=dy; args[8]=w; args[9]=h; args[10]=mode;
            }
            /* identical starting surface contents */
            uint32_t nbytes=fs.pitch*sh+64; if(nbytes>MAXSURF) nbytes=MAXSURF;
            for(uint32_t i=0;i<nbytes;i+=4) wr32(fs.pix+i,0xA5A5A5A5u^(i*2654435761u));
            memcpy(snapA,gmem+fs.pix,nbytes);
            uint32_t regsA[5], regsB[5];
            /* modes > handled range leave uninitialised stack garbage in the original -> only compare handled modes */
            int comparable = (which==0)? (mode<=3) : (mode<=2);
            rbo_uninstall_hooks();
            n_lock=n_unlock=0;
            uint32_t rA=run(which==0?0x414d10:0x415060, which==0?8:11, args, regsA);
            static uint8_t outA[MAXSURF+64]; memcpy(outA,gmem+fs.pix,nbytes);
            int lA=n_lock,uA=n_unlock;
            memcpy(gmem+fs.pix,snapA,nbytes);
            rbo_install_hooks_mask(which==0?1:2);
            n_lock=n_unlock=0;
            uint32_t rB=run(which==0?0x414d10:0x415060, which==0?8:11, args, regsB);
            int lB=n_lock,uB=n_unlock;
            int bad=0;
            if(comparable && memcmp(outA,gmem+fs.pix,nbytes)) bad|=1;
            if(rA!=rB) bad|=2;
            if(memcmp(regsA,regsB,sizeof regsA)) bad|=4;
            if(lA!=lB||uA!=uB) bad|=8;
            if(!comparable){ /* fallback must still be identical except for garbage-driven bytes: require only regs/ret/lock */ bad&=~1; }
            cases++; per_cases++;
            if(bad){ fails++; if(fails<12) fprintf(stderr,"FAIL %s it=%d fmt=%d mode=%u w=%u h=%u dx=%u dy=%u pitch=%u bad=%x ret %u/%u locks %d/%d unlocks %d/%d\n",nm[which],it,f,mode,w,h,dx,dy,fs.pitch,bad,rA,rB,lA,lB,uA,uB); }
        }
        fprintf(stderr,"%-14s %d cases, %d failures\n",nm[which],per_cases,fails-per_fail_before);
    }

    /* ---- scan hook: tables of slots, stub the two callee functions so both runs are deterministic ---- */
    {
        int bad_scan=0, n_scan=0;
        /* stubs in the (private) guest image: 0x466820: mov eax,[esp+4]; ret   0x466880: xor eax,eax; ret */
        static const uint8_t st1[]={0x8b,0x44,0x24,0x04,0xc3}, st2[]={0x31,0xc0,0xc3};
        memcpy(gmem+0x466820,st1,sizeof st1); memcpy(gmem+0x466880,st2,sizeof st2);
        for(int it=0;it<iters;it++){
            uint32_t nent=1+rnd()%4, tab=ga_alloc(nent*0x20,1);
            wr32(0x2553b10,nent); wr32(0x2553b14,tab);
            for(uint32_t e=0;e<nent;e++){
                uint32_t cnt=rnd()%3?rnd()%3000:0; if(rnd()%5==0) cnt=1+rnd()%4;
                uint32_t a=ga_alloc(8*(cnt+4)+0x100,1);
                wr32(tab+e*0x20+0,rnd()%50); wr32(tab+e*0x20+4,1+rnd()%3); wr32(tab+e*0x20+8,cnt);
                for(int k=3;k<=7;k++){ wr32(tab+e*0x20+4*k, a+((k-3)*(cnt+4)*4>>0)%(0x100)*0 + 0); }
                /* separate arrays for slots(+0xc) and the four side arrays */
                for(int k=3;k<=7;k++){ uint32_t arr=ga_alloc(4*(cnt+4),1); wr32(tab+e*0x20+4*k,arr); }
                uint32_t slots=rd32(tab+e*0x20+0xc);
                for(uint32_t i=0;i<cnt;i++){ uint32_t dens=rnd()%4; wr32(slots+4*i, (rnd()%(dens==0?2:dens==1?20:400))==0? (1+rnd()%1000) : 0xFFFFFFFFu); }
                for(int k=4;k<=7;k++){ uint32_t arr=rd32(tab+e*0x20+4*k); for(uint32_t i=0;i<cnt;i++) wr32(arr+4*i,rnd()); }
            }
            /* snapshot all memory the function can touch (tables); run A then B on a copy */
            uint32_t regsA[5],regsB[5];
            /* record table memory */
            uint32_t tot=0; static uint8_t snapT[1<<22]; (void)tot;
            /* copy state: since writes are only into the slot/side arrays (<= few hundred KB), snapshot the heap window */
            uint32_t lo=tab, hi=ga_alloc(16,0)+16; if(hi-lo>sizeof snapT) hi=lo+sizeof snapT;
            memcpy(snapT,gmem+lo,hi-lo);
            rbo_uninstall_hooks();
            run(0x467130,0,NULL,regsA);
            static uint8_t outT[1<<22]; memcpy(outT,gmem+lo,hi-lo);
            memcpy(gmem+lo,snapT,hi-lo);
            rbo_install_hooks_mask(4);
            uint64_t ic0=cpu.icount; run(0x467130,0,NULL,regsB); (void)ic0;
            n_scan++;
            if(memcmp(outT,gmem+lo,hi-lo)||memcmp(regsA,regsB,sizeof regsA)){ bad_scan++; if(bad_scan<5) fprintf(stderr,"FAIL scan it=%d\n",it); }
            rbo_uninstall_hooks();
        }
        fprintf(stderr,"scan 0x467130  %d cases, %d failures\n",n_scan,bad_scan);
        fails+=bad_scan;
    }
    fprintf(stderr,"TOTAL: %s (%d failures)\n",fails?"FAILED":"ALL IDENTICAL",fails);
    return fails?1:0;
}
