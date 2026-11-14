/* test_gpu.c - checks d3d.c's GPU routing against the CPU rasterizer.  gpu_stub.c replaces Metal with a float software "GPU" that follows the
 * shader's maths, so this verifies everything on the C side: state mapping, vertex building, culling, texture upload/colour key, render-target
 * upload/readback and CPU<->GPU hand-over (blits between draws).  Small differences (rounding) are expected; large ones are bugs.
 *   make test_gpu && ./test_gpu [iterations]                                                                                                  */
#define RBO_GPU 1
#include "d3d.c"
uint8_t *gmem; int g_cur_idx; const char *g_root=NULL; int g_frames=1;
static Surf fake[4];
Surf *surf_of(uint32_t obj){ return obj? &fake[obj&3] : NULL; }
uint32_t make_vtable(Iface *i){ (void)i; return 0; }
void com_default(void){} void com_addref(void){} void com_release(void){}

static uint64_t rs_=88172645463325252ull;
static uint32_t rnd(void){ rs_^=rs_<<13; rs_^=rs_>>7; rs_^=rs_<<17; return (uint32_t)(rs_>>11); }
static float rf(float lo,float hi){ return lo+(hi-lo)*(rnd()&0xFFFFFF)/16777215.0f; }

#define RW 48
#define RH 40
#define VBASE 0x100000u
typedef struct { int kind; /* 0 draw, 1 cpu pokes, 2 clear rect */
                 int nv; float v[3*9*8]; int l,t,r,b; uint32_t color; int px[6]; uint32_t pv[6]; } Op;

static void play(const Op *ops,int nops,int gpu,const uint8_t *init,uint8_t *out,int type,uint32_t fvf){
    Surf *rt=&fake[0]; size_t nb=(size_t)rt->pitch*rt->h;
    memcpy(gmem+rt->pix,init,nb);
    gpu_mode=gpu?2:0; gpu_dead=0; pol_use=gpu; pol_frame=g_frames; g_res=NULL; g_res_valid=0; g_res_newer=0; fake[1].ggen=0;
    for(int i=0;i<nops;i++){ const Op *o=&ops[i];
        if(o->kind==0){
            for(int k=0;k<o->nv;k++){ uint8_t *p=gmem+VBASE+k*28; float *f=(float*)p; f[0]=o->v[k*7+0]; f[1]=o->v[k*7+1]; f[2]=0.5f; f[3]=1.0f;
                *(uint32_t*)(p+16)=*(const uint32_t*)&o->v[k*7+2]; f[5]=o->v[k*7+3]; f[6]=o->v[k*7+4]; }
            draw(type,fvf,VBASE,o->nv,0,0);
        } else if(o->kind==1){
            gpu_surf_access(rt,1);
            for(int k=0;k<6;k++) put_raw(rt,o->px[k]%RW,o->px[k]/RW%RH,o->pv[k]&(rt->rmask|rt->gmask|rt->bmask|rt->amask));
        } else {
            if(!(gpu&&gpu_clear_rect(rt,o->l,o->t,o->r,o->b,o->color))){
                gpu_surf_access(rt,1); uint32_t px=from_argb(rt,o->color);
                for(int y=o->t;y<o->b&&y<rt->h;y++) for(int x=o->l;x<o->r&&x<rt->w;x++) if(x>=0&&y>=0) put_raw(rt,x,y,px); }
        }
    }
    gpu_surf_access(rt,0);
    memcpy(out,gmem+rt->pix,nb);
}

int main(int argc,char **argv){
    int iters=argc>1?atoi(argv[1]):300;
    gmem=calloc(1,64<<20);
    static const struct{ int bpp; uint32_t r,g,b,a; int minbits; } F[]={ {16,0xF800,0x07E0,0x001F,0,5},{16,0x0F00,0x00F0,0x000F,0xF000,4},{16,0x7C00,0x03E0,0x001F,0x8000,5},
        {32,0xFF0000,0xFF00,0xFF,0xFF000000u,8},{32,0xFF0000,0xFF00,0xFF,0,8},{32,0xFF,0xFF00,0xFF0000,0xFF000000u,8} };
    long pix_total=0,pix_bad=0,pix_big=0; int worst=0; long draws_gpu_total=0; int cfg_fail=0;
    for(int it=0;it<iters;it++){
        Surf *rt=&fake[0], *tx=&fake[1]; memset(rt,0,sizeof *rt); memset(tx,0,sizeof *tx);
        int fi=rnd()%6; rt->used=1; rt->id=0; rt->w=RW; rt->h=RH; rt->bpp=F[fi].bpp; rt->pitch=RW*(rt->bpp/8); rt->pix=0x1000;
        rt->rmask=F[fi].r; rt->gmask=F[fi].g; rt->bmask=F[fi].b; rt->amask=F[fi].a;
        tx->used=1; tx->id=1; tx->w=1+rnd()%40; if(rnd()%2) tx->w=1<<(rnd()%6); tx->h=1+rnd()%40; if(rnd()%2) tx->h=1<<(rnd()%6);
        tx->bpp=32; tx->ck_valid=rnd()%3==0; size_t n=(size_t)tx->w*tx->h;
        tx->cache=malloc(n*4); tx->keymask=malloc(n); tx->cache_cap=(int)n; tx->cache_ok=1; tx->cgen=(uint32_t)it+1; tx->ggen=0;
        for(size_t i=0;i<n;i++){ uint32_t c=rnd(); uint32_t m=rnd()%5; if(m==0) c|=0xFF000000u; else if(m==1) c&=0x00FFFFFFu; tx->cache[i]=c; tx->keymask[i]=(rnd()%7==0); }
        memset(&D,0,sizeof D); D.vpw=RW; D.vph=RH; if(rnd()%5==0){ D.vpx=rnd()%10; D.vpy=rnd()%10; D.vpw=RW-D.vpx-rnd()%8; D.vph=RH-D.vpy-rnd()%8; }
        D.rs[22]=1+rnd()%3;
        D.rs[27]=rnd()%3!=0; D.rs[19]=rnd()%2?5:1+rnd()%10; D.rs[20]=rnd()%2?6:1+rnd()%10; D.rs[15]=rnd()%3==0; D.rs[25]=1+rnd()%8; D.rs[24]=rnd()&255; D.rs[60]=rnd();
        D.tss[0][1]=rnd()%4==0?1+rnd()%8:4; D.tss[0][2]=rnd()%2?2:rnd()%8; D.tss[0][3]=rnd()%2?0:rnd()%8; D.tss[0][4]=rnd()%4==0?1+rnd()%8:4; D.tss[0][5]=rnd()%2?2:rnd()%8; D.tss[0][6]=rnd()%2?0:rnd()%8;
        D.tss[0][12]=rnd()%4; D.tss[0][16]=rnd()%3; D.rs[17]=rnd()%3; D.rs[41]=rnd()%2; D.tex[0]=rnd()%10==0?0:1;
        D.target=1+2; /* fake[3&3]... not used: draw() uses surf_of(D.target) */
        D.target=4;   /* 4&3 == 0 -> fake[0] */
        if(getenv("NOBLEND")) D.rs[27]=0; if(getenv("NOKEY")){ D.rs[41]=0; tx->ck_valid=0; } if(getenv("NOTEX")) D.tex[0]=0; if(getenv("NOATEST")) D.rs[15]=0;
        if(getenv("POINT")){ D.tss[0][16]=0; D.rs[17]=0; } if(getenv("LINEAR")){ D.tss[0][16]=2; } if(getenv("MOD")){ D.tss[0][1]=4; D.tss[0][2]=2; D.tss[0][3]=0; D.tss[0][4]=4; D.tss[0][5]=2; D.tss[0][6]=0; }
        nfmt=0; gexp16_ok=0; exp16_r=exp16_g=exp16_b=0xFFFFFFFFu;
        /* initial render target contents */
        size_t nb=(size_t)rt->pitch*rt->h; static uint8_t init[RW*RH*4+64], outc[RW*RH*4+64], outg[RW*RH*4+64];
        for(size_t i=0;i<nb;i++) init[i]=rnd();
        for(int y=0;y<RH;y++) for(int x=0;x<RW;x++){ uint8_t *p=init+y*rt->pitch+x*(rt->bpp/8); uint32_t m=rt->rmask|rt->gmask|rt->bmask|rt->amask;
            if(rt->bpp==16) *(uint16_t*)p&=m; else *(uint32_t*)p&=m; }
        /* script */
        static Op ops[16]; int nops=2+rnd()%6;
        for(int i=0;i<nops;i++){ Op *o=&ops[i]; memset(o,0,sizeof *o); int r=rnd()%10; o->kind=r<7?0:(r<9?1:2);
            if(o->kind==0){ o->nv=3*(1+rnd()%6); int flat=rnd()%10<6; uint32_t base=rnd();
                for(int k=0;k<o->nv;k++){ float *v=&o->v[k*7]; v[0]=rf(-6,RW+6); v[1]=rf(-6,RH+6); uint32_t c=flat?base:rnd();
                    switch(rnd()%4){ case 0: c|=0xFF000000u; break; case 1: c&=0x00FFFFFFu; break; default: break; } memcpy(&v[2],&c,4); v[3]=rf(-1,2); v[4]=rf(-1,2); } }
            else if(o->kind==1){ for(int k=0;k<6;k++){ o->px[k]=rnd()%(RW*RH); o->pv[k]=rnd(); } }
            else { o->l=rnd()%RW; o->t=rnd()%RH; o->r=o->l+1+rnd()%RW; o->b=o->t+1+rnd()%RH; o->color=rnd(); }
        }
        if(getenv("NOPS")&&it==atoi(getenv("DBG")?getenv("DBG"):"-1")) nops=atoi(getenv("NOPS"));
        if(getenv("DBG")&&it==atoi(getenv("DBG"))){ for(int i=0;i<nops;i++) printf(" op%d kind=%d nv=%d\n",i,ops[i].kind,ops[i].nv); }
        int type=4; uint32_t fvf=4|0x40|(1<<8);
        play(ops,nops,0,init,outc,type,fvf);
        unsigned long g0=st_gdraws; play(ops,nops,1,init,outg,type,fvf); draws_gpu_total+=st_gdraws-g0;
        /* compare in expanded ARGB */
        int thr=(255>>(8-F[fi].minbits))+3; if(F[fi].bpp==32) thr=4; long bad=0;
        for(int y=0;y<RH;y++) for(int x=0;x<RW;x++){
            uint32_t a=to_argb(rt, rt->bpp==16? *(uint16_t*)(outc+y*rt->pitch+x*2) : *(uint32_t*)(outc+y*rt->pitch+x*4));
            uint32_t b=to_argb(rt, rt->bpp==16? *(uint16_t*)(outg+y*rt->pitch+x*2) : *(uint32_t*)(outg+y*rt->pitch+x*4));
            int mx=0; for(int ch=0;ch<4;ch++){ int d=abs((int)((a>>(ch*8))&255)-(int)((b>>(ch*8))&255)); if(rt->amask==0&&ch==3) d=0; if(d>mx) mx=d; }
            if(mx>thr && getenv("DBG") && it==atoi(getenv("DBG")) && bad<12) printf("  it=%d (%d,%d) cpu=%08x gpu=%08x\n",it,x,y,a,b);
            pix_total++; if(mx>thr){ pix_bad++; bad++; } if(mx>worst) worst=mx; if(mx>2*thr+8) pix_big++;
        }
        if(bad>RW*RH/20){ cfg_fail++; if(cfg_fail<=8) printf("CONFIG DIFF it=%d fmt=%d bad=%ld  tex=%d cop=%d aop=%d ca=%d/%d aa=%d/%d blend=%d sf=%d df=%d atest=%d/%d/%d lin=%d/%d addr=%d key=%d/%d cull=%d vp=%d,%d,%d,%d\n",
            it,fi,bad,D.tex[0],D.tss[0][1],D.tss[0][4],D.tss[0][2],D.tss[0][3],D.tss[0][5],D.tss[0][6],D.rs[27],D.rs[19],D.rs[20],D.rs[15],D.rs[25],D.rs[24],D.tss[0][16],D.rs[17],D.tss[0][12],D.rs[41],tx->ck_valid,D.rs[22],D.vpx,D.vpy,D.vpw,D.vph); }
        free(tx->cache); free(tx->keymask);
    }
    printf("%ld pixels compared, %ld beyond tolerance (%.3f%%), %ld far off, worst channel diff %d; %ld draws went through the GPU path; %d configs with >5%% bad pixels\n",
        pix_total,pix_bad,100.0*pix_bad/pix_total,pix_big,worst,draws_gpu_total,cfg_fail);
    return (cfg_fail||pix_big>pix_total/2000)?1:0;
}
