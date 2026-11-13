/* test_raster.c - fuzz: the optimised pixel path (shade_px2) must write exactly the same bytes as the original (shade_px_ref).
 *   make test_raster && ./test_raster [iterations]                                                                          */
#define RBO_RASTCHECK
#include "d3d.c"
uint8_t *gmem; int g_cur_idx;
static Surf fake[4];
Surf *surf_of(uint32_t obj){ return obj? &fake[obj&3] : NULL; }
uint32_t make_vtable(Iface *i){ (void)i; return 0; }
void com_default(void){} void com_addref(void){} void com_release(void){}

static uint64_t rs_=88172645463325252ull;
static uint32_t rnd(void){ rs_^=rs_<<13; rs_^=rs_>>7; rs_^=rs_<<17; return (uint32_t)(rs_>>11); }
static float rf(float lo,float hi){ return lo+(hi-lo)*(rnd()&0xFFFFFF)/16777215.0f; }

int main(int argc,char **argv){
    int iters=argc>1?atoi(argv[1]):20000;
    /* 1. the shortcuts rely on these identities */
    int bad=0; for(uint32_t x=0;x<256;x++){ if(mul8(255,x)!=x||mul8(x,255)!=x||mul8(x,0)!=0) bad++; }
    printf("mul8 identities: %s\n",bad?"FAILED":"hold for all 0..255");
    if(bad) return 1;
    gmem=calloc(1,64<<20);
    static const struct{ int bpp; uint32_t r,g,b,a; } F[]={ {16,0xF800,0x07E0,0x001F,0},{16,0x0F00,0x00F0,0x000F,0xF000},{16,0x7C00,0x03E0,0x001F,0x8000},
        {32,0xFF0000,0xFF00,0xFF,0xFF000000u},{32,0xFF0000,0xFF00,0xFF,0},{32,0xFF,0xFF00,0xFF0000,0xFF000000u} };
    long total=0, fails=0, white_hits=0;
    for(int it=0;it<iters;it++){
        Surf *rt=&fake[0], *tx=&fake[1]; memset(rt,0,sizeof *rt); memset(tx,0,sizeof *tx);
        int fi=rnd()%6; rt->used=1; rt->w=32; rt->h=32; rt->bpp=F[fi].bpp; rt->pitch=32*(rt->bpp/8); rt->pix=0x1000;
        rt->rmask=F[fi].r; rt->gmask=F[fi].g; rt->bmask=F[fi].b; rt->amask=F[fi].a;
        tx->used=1; tx->w=1+rnd()%40; if(rnd()%2) tx->w=1<<(rnd()%6); tx->h=1+rnd()%40; if(rnd()%2) tx->h=1<<(rnd()%6);
        tx->bpp=32; tx->ck_valid=rnd()%3==0; tx->ck_lo=0; tx->ck_hi=0;
        size_t n=(size_t)tx->w*tx->h; tx->cache=malloc(n*4); tx->keymask=malloc(n); tx->cache_cap=(int)n; tx->cache_ok=1;
        for(size_t i=0;i<n;i++){ uint32_t c=rnd(); uint32_t m=rnd()%5; if(m==0) c|=0xFF000000u; else if(m==1) c&=0x00FFFFFFu; tx->cache[i]=c; tx->keymask[i]=(rnd()%7==0); }
        memset(&D,0,sizeof D); D.vpw=32; D.vph=32;
        D.rs[27]=rnd()%3!=0; D.rs[19]=rnd()%2?5:1+rnd()%10; D.rs[20]=rnd()%2?6:1+rnd()%10; D.rs[15]=rnd()%3==0; D.rs[25]=1+rnd()%8; D.rs[24]=rnd()&255; D.rs[60]=rnd();
        D.tss[0][1]=rnd()%4==0?1+rnd()%8:4; D.tss[0][2]=rnd()%2?2:rnd()%8; D.tss[0][3]=rnd()%2?0:rnd()%8; D.tss[0][4]=rnd()%4==0?1+rnd()%8:4; D.tss[0][5]=rnd()%2?2:rnd()%8; D.tss[0][6]=rnd()%2?0:rnd()%8;
        D.tss[0][12]=rnd()%4; D.tss[0][16]=rnd()%3; D.rs[17]=rnd()%3; D.rs[41]=rnd()%2;
        D.tex[0]=1+ (rnd()%8==0?0:0);
        if(rnd()%10==0) D.tex[0]=0;
        nfmt=0; exp16_r=exp16_g=exp16_b=0xFFFFFFFFu;
        rc_setup(rt);
        if(rc.white_ok) white_hits++;
        for(int k=0;k<100;k++){
            int x=rnd()%32, y=rnd()%32; uint32_t diff=rnd();
            switch(rnd()%5){ case 0: case 1: diff=0xFFFFFFFFu; break; case 2: diff|=0xFF000000u; break; case 3: diff&=0x00FFFFFFu; break; default: break; }
            float u,v; if(rnd()%3==0){ u=(float)(rnd()%(tx->w*2))/(float)tx->w + 0.5f/tx->w; v=(float)(rnd()%(tx->h*2))/(float)tx->h + 0.5f/tx->h; } else { u=rf(-2,3); v=rf(-2,3); }
            uint8_t *p=gmem+rt->pix+y*rt->pitch+x*(rt->bpp/8);
            for(int b=0;b<4;b++) p[b]=rnd();
            uint8_t snap[4]; memcpy(snap,p,4);
            shade_px_ref(x,y,diff,u,v); uint8_t ra[4]; memcpy(ra,p,4);
            memcpy(p,snap,4);
            shade_px2(&rc,x,y,diff,u,v); uint8_t rb[4]; memcpy(rb,p,4);
            total++; if(memcmp(ra,rb,4)){ fails++; if(fails<10) printf("MISMATCH it=%d fmt=%d cop=%d aop=%d ca=%d/%d aa=%d/%d blend=%d sf=%d df=%d diff=%08x u=%f v=%f ref=%02x%02x%02x%02x new=%02x%02x%02x%02x\n",it,fi,rc.cop,rc.aop,rc.ca1,rc.ca2,rc.aa1,rc.aa2,rc.blend,rc.sf,rc.df,diff,u,v,ra[0],ra[1],ra[2],ra[3],rb[0],rb[1],rb[2],rb[3]); }
        }
        free(tx->cache); free(tx->keymask);
    }
    printf("%ld pixels compared (%ld in white-diffuse fast-path configs), %ld mismatches -> %s\n",total,white_hits*100,fails,fails?"FAILED":"IDENTICAL");
    return fails?1:0;
}
