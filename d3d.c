/* Direct3D 7 HLE with a small software rasterizer (reference implementation for validation) */
#include "dxshared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern int g_cur_idx;

/* ---------- pixel helpers ---------- */
static int sh_of(uint32_t m){ int s=0; if(!m) return 0; while(!(m&1)){m>>=1;s++;} return s; }
static int bits_of(uint32_t m){ int n=0; if(!m) return 0; m>>=sh_of(m); while(m&1){n++;m>>=1;} return n; }
static uint32_t raw_px(Surf*s,int x,int y){
    uint8_t *p=gmem+s->pix+y*s->pitch+x*(s->bpp/8);
    if(s->bpp==16) return *(uint16_t*)p; if(s->bpp==32) return *(uint32_t*)p;
    if(s->bpp==24) return p[0]|(p[1]<<8)|(p[2]<<16); return *p;
}
static void put_raw(Surf*s,int x,int y,uint32_t v){
    uint8_t *p=gmem+s->pix+y*s->pitch+x*(s->bpp/8);
    if(s->bpp==16) *(uint16_t*)p=v; else if(s->bpp==32) *(uint32_t*)p=v; else if(s->bpp==24){p[0]=v;p[1]=v>>8;p[2]=v>>16;} else *p=v;
}
typedef struct { uint32_t rm,gm,bm,am; int rs,gs,bs,as,rb,gb,bb,ab; } Fmt;
static Fmt fmts[16]; static int nfmt=0;
static const Fmt *fmt_of(Surf*s){
    for(int i=0;i<nfmt;i++) if(fmts[i].rm==s->rmask&&fmts[i].gm==s->gmask&&fmts[i].bm==s->bmask&&fmts[i].am==s->amask) return &fmts[i];
    Fmt *f=&fmts[nfmt<15?nfmt++:15]; f->rm=s->rmask; f->gm=s->gmask; f->bm=s->bmask; f->am=s->amask;
    f->rs=sh_of(f->rm); f->gs=sh_of(f->gm); f->bs=sh_of(f->bm); f->as=sh_of(f->am);
    f->rb=bits_of(f->rm); f->gb=bits_of(f->gm); f->bb=bits_of(f->bm); f->ab=bits_of(f->am); return f;
}
static inline uint32_t c8(uint32_t px,uint32_t m,int sh,int n){ if(!m) return 255; uint32_t v=(px&m)>>sh; return n>=8? v>>(n-8) : (v*255)/((1u<<n)-1); }
static uint32_t to_argb(Surf*s,uint32_t px){ const Fmt *f=fmt_of(s); return (c8(px,f->am,f->as,f->ab)<<24)|(c8(px,f->rm,f->rs,f->rb)<<16)|(c8(px,f->gm,f->gs,f->gb)<<8)|c8(px,f->bm,f->bs,f->bb); }
static inline uint32_t pc(uint32_t v8,uint32_t m,int sh,int n){ if(!m) return 0; return ((n>=8? v8<<(n-8) : v8>>(8-n))<<sh)&m; }
static uint32_t from_argb(Surf*s,uint32_t a){ const Fmt *f=fmt_of(s); return pc(a>>24,f->am,f->as,f->ab)|pc((a>>16)&255,f->rm,f->rs,f->rb)|pc((a>>8)&255,f->gm,f->gs,f->gb)|pc(a&255,f->bm,f->bs,f->bb); }

/* ---------- device state ---------- */
typedef struct { float x,y,z,rhw; uint32_t col; float u,v; } Vtx;
typedef struct {
    uint32_t target;           /* guest surface object */
    int vpx,vpy,vpw,vph;
    uint32_t rs[256];
    uint32_t tss[8][32];
    uint32_t tex[8];
    float world[16],view[16],proj[16];
    int frames_drawn, draws;
} Dev;
static Dev D;
static uint32_t vt_d3d=0, vt_dev=0, vt_vb=0, d3d_obj=0, dev_obj=0;

static void ident(float *m){ memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; }
static void dev_defaults(void){
    memset(D.rs,0,sizeof D.rs); memset(D.tss,0,sizeof D.tss);
    D.rs[19]=2; D.rs[20]=1; D.rs[22]=3; D.rs[25]=8; D.rs[27]=0; D.rs[7]=0; D.rs[14]=1;
    D.tss[0][1]=4; D.tss[0][2]=2; D.tss[0][3]=0; D.tss[0][4]=2; D.tss[0][5]=2; D.tss[0][6]=0;
    D.tss[0][12]=1; D.tss[0][16]=1; D.tss[0][17]=1;
    for(int i=1;i<8;i++) D.tss[i][1]=1;
    ident(D.world); ident(D.view); ident(D.proj);
}

/* ---------- rasterizer ---------- */
typedef struct {
    Surf *rt; int rbpp; const Fmt *rf;
    int blend, sf, df, atest, afunc, aref;
    int tex_on; const uint32_t *tex; const uint8_t *key; int tw,th,tmask_w,tmask_h; int addr, linear, use_key;
    int cop,aop,ca1,ca2,aa1,aa2; uint32_t tfac;
    uint32_t *exp16; int rmode;   /* destination fast conversion */
} RC;
static RC rc;
static uint32_t exp16_tab[65536]; static uint32_t exp16_r=0,exp16_g=0,exp16_b=0;

static void tex_cache_build(Surf *t){
    size_t n=(size_t)t->w*t->h;
    if(t->cache_cap<(int)n){ free(t->cache); free(t->keymask); t->cache=malloc(n*4); t->keymask=malloc(n); t->cache_cap=(int)n; }
    const Fmt *f=fmt_of(t);
    for(int y=0;y<t->h;y++) for(int x=0;x<t->w;x++){
        uint32_t raw=raw_px(t,x,y); size_t i=(size_t)y*t->w+x;
        t->cache[i]=to_argb(t,raw);
        t->keymask[i]=(t->ck_valid&&raw>=t->ck_lo&&raw<=t->ck_hi);
    }
    (void)f; t->cache_ok=1;
}
static void rc_setup(Surf *rt){
    memset(&rc,0,sizeof rc);
    rc.rt=rt; rc.rbpp=rt->bpp; rc.rf=fmt_of(rt);
    rc.blend=D.rs[27]; rc.sf=D.rs[19]; rc.df=D.rs[20]; rc.atest=D.rs[15]; rc.afunc=D.rs[25]; rc.aref=D.rs[24]&255;
    rc.cop=D.tss[0][1]; rc.aop=D.tss[0][4]; rc.ca1=D.tss[0][2]&7; rc.ca2=D.tss[0][3]&7; rc.aa1=D.tss[0][5]&7; rc.aa2=D.tss[0][6]&7; rc.tfac=D.rs[60];
    if(D.tex[0] && rc.cop!=1){
        Surf *t=surf_of(D.tex[0]);
        if(!t->cache_ok) tex_cache_build(t);
        rc.tex_on=1; rc.tex=t->cache; rc.key=t->keymask; rc.tw=t->w; rc.th=t->h;
        rc.tmask_w=((t->w&(t->w-1))==0)?t->w-1:-1; rc.tmask_h=((t->h&(t->h-1))==0)?t->h-1:-1;
        rc.addr=D.tss[0][12]?D.tss[0][12]:1; rc.linear=(D.tss[0][16]==2||D.rs[17]==2); rc.use_key=D.rs[41]&&t->ck_valid;
    }
    if(rt->bpp==16){
        if(exp16_r!=rt->rmask||exp16_g!=rt->gmask||exp16_b!=rt->bmask){
            for(uint32_t v=0;v<65536;v++) exp16_tab[v]=to_argb(rt,v);
            exp16_r=rt->rmask; exp16_g=rt->gmask; exp16_b=rt->bmask;
        }
        rc.exp16=exp16_tab;
    }
}
static inline int wrapc(int i,int n,int mask,int addr){
    if(addr==3){ return i<0?0:(i>=n?n-1:i); }
    if(addr==2){ int p=((i%(2*n))+2*n)%(2*n); return p<n?p:2*n-1-p; }
    return mask>=0 ? (i&mask) : (((i%n)+n)%n);
}
static inline uint32_t tfetch(int x,int y,int *kill){
    size_t i=(size_t)y*rc.tw+x; if(rc.use_key&&rc.key[i]) *kill=1; return rc.tex[i];
}
static inline uint32_t tsample(float u,float v,int *kill){
    float fx=u*rc.tw, fy=v*rc.th; *kill=0;
    if(!rc.linear){
        int x=wrapc((int)floorf(fx),rc.tw,rc.tmask_w,rc.addr), y=wrapc((int)floorf(fy),rc.th,rc.tmask_h,rc.addr);
        return tfetch(x,y,kill);
    }
    fx-=0.5f; fy-=0.5f; int x0=(int)floorf(fx), y0=(int)floorf(fy); float ax=fx-x0, ay=fy-y0;
    int xs[2]={wrapc(x0,rc.tw,rc.tmask_w,rc.addr),wrapc(x0+1,rc.tw,rc.tmask_w,rc.addr)}, ys[2]={wrapc(y0,rc.th,rc.tmask_h,rc.addr),wrapc(y0+1,rc.th,rc.tmask_h,rc.addr)};
    int kk=0, kc=0; uint32_t c[4]; int idx=0;
    for(int j=0;j<2;j++) for(int i=0;i<2;i++){ int k=0; c[idx]=tfetch(xs[i],ys[j],&k); if(k){ kc++; c[idx]=0; } idx++; }
    (void)kk;
    uint32_t w0=(uint32_t)((1-ax)*(1-ay)*256), w1=(uint32_t)(ax*(1-ay)*256), w2=(uint32_t)((1-ax)*ay*256), w3=256-w0-w1-w2; uint32_t o=0;
    for(int ch=0;ch<4;ch++){ uint32_t acc=w0*((c[0]>>(ch*8))&255)+w1*((c[1]>>(ch*8))&255)+w2*((c[2]>>(ch*8))&255)+w3*((c[3]>>(ch*8))&255); o|=((acc+128)>>8)<<(ch*8); }
    if(kc==4) *kill=1;
    return o;
}
static inline uint32_t mul8(uint32_t a,uint32_t b){ uint32_t t=a*b+128; return (t+(t>>8))>>8; }
static inline uint32_t sel(int a,uint32_t diff,uint32_t tex){ switch(a){ case 2: return tex; case 3: return rc.tfac; default: return diff; } }
static inline uint32_t cmodc(uint32_t a,uint32_t b,int sh){
    uint32_t r0=mul8(a&255,b&255)<<sh, r1=mul8((a>>8)&255,(b>>8)&255)<<sh, r2=mul8((a>>16)&255,(b>>16)&255)<<sh, r3=mul8(a>>24,b>>24)<<sh;
    if(r0>255)r0=255; if(r1>255)r1=255; if(r2>255)r2=255; if(r3>255)r3=255; return r0|(r1<<8)|(r2<<16)|(r3<<24);
}
static inline uint32_t caddc(uint32_t a,uint32_t b){ uint32_t o=0; for(int i=0;i<4;i++){ uint32_t x=((a>>(i*8))&255)+((b>>(i*8))&255); if(x>255)x=255; o|=x<<(i*8);} return o; }
static inline int bfac(int f,uint32_t sc,uint32_t dc,int ch){
    uint32_t sa=sc>>24, da=dc>>24; int s=(sc>>(ch*8))&255, d=(dc>>(ch*8))&255;
    switch(f){ case 1:return 0; case 2:return 255; case 3:return s; case 4:return 255-s; case 5:return sa; case 6:return 255-sa; case 7:return da; case 8:return 255-da; case 9:return d; case 10:return 255-d; default:return 255; }
}
static inline int acmp2(int fn,int a,int ref){ switch(fn){ case 1:return 0; case 2:return a<ref; case 3:return a==ref; case 4:return a<=ref; case 5:return a>ref; case 6:return a!=ref; case 7:return a>=ref; default:return 1; } }
static inline uint32_t packrt(uint32_t a){
    const Fmt *f=rc.rf;
    return (f->rm? ((((a>>16)&255)>>(8-f->rb))<<f->rs):0)|(f->gm? ((((a>>8)&255)>>(8-f->gb))<<f->gs):0)|(f->bm? (((a&255)>>(8-f->bb))<<f->bs):0)|(f->am? (((a>>24)>>(8-f->ab))<<f->as):0);
}
static inline void shade_px(int x,int y,uint32_t diff,float u,float v){
    uint32_t col=diff;
    if(rc.tex_on){
        int kill; uint32_t tx=tsample(u,v,&kill); if(kill) return;
        uint32_t c1=sel(rc.ca1,diff,tx), c2=sel(rc.ca2,diff,tx), cc;
        switch(rc.cop){ case 2: cc=c1; break; case 3: cc=c2; break; case 5: cc=cmodc(c1,c2,1); break; case 6: cc=cmodc(c1,c2,2); break; case 7: cc=caddc(c1,c2); break; default: cc=cmodc(c1,c2,0); }
        uint32_t a1=sel(rc.aa1,diff,tx)>>24, a2=sel(rc.aa2,diff,tx)>>24, aa;
        switch(rc.aop){ case 1: aa=diff>>24; break; case 2: aa=a1; break; case 3: aa=a2; break; case 5: aa=mul8(a1,a2)*2; break; case 7: aa=a1+a2; break; default: aa=mul8(a1,a2); }
        if(aa>255) aa=255; col=(aa<<24)|(cc&0xFFFFFF);
    }
    if(rc.atest && !acmp2(rc.afunc,col>>24,rc.aref)) return;
    Surf *rt=rc.rt; uint8_t *p=gmem+rt->pix+y*rt->pitch+x*(rt->bpp/8);
    uint32_t out=col;
    if(rc.blend){
        uint32_t dst = rc.rbpp==16 ? rc.exp16[*(uint16_t*)p] : to_argb(rt,*(uint32_t*)p);
        int sf=rc.sf, df=rc.df; uint32_t r=col&0xFF000000u;
        if(sf==5&&df==6){ uint32_t sa=col>>24, ia=255-sa;
            for(int ch=0;ch<3;ch++){ uint32_t s=(col>>(ch*8))&255, d=(dst>>(ch*8))&255; uint32_t val=mul8(s,sa)+mul8(d,ia); if(val>255)val=255; r|=val<<(ch*8); } }
        else for(int ch=0;ch<3;ch++){ int s=(col>>(ch*8))&255, d=(dst>>(ch*8))&255; int val=(s*bfac(sf,col,dst,ch)+d*bfac(df,col,dst,ch))/255; if(val>255)val=255; r|=(uint32_t)val<<(ch*8); }
        out=r;
    }
    uint32_t px=packrt(out);
    if(rc.rbpp==16) *(uint16_t*)p=(uint16_t)px; else if(rc.rbpp==32) *(uint32_t*)p=px; else put_raw(rt,x,y,px);
}
static inline float edge(float ax,float ay,float bx,float by,float px,float py){ return (bx-ax)*(py-ay)-(by-ay)*(px-ax); }
static int is_tl(float ax,float ay,float bx,float by){ float dx=bx-ax, dy=by-ay; return (dy==0&&dx>0)||dy<0; }
static void raster_tri(Surf*rt,Vtx a,Vtx b,Vtx c){
    float area=edge(a.x,a.y,b.x,b.y,c.x,c.y); if(area==0) return;
    int cull=D.rs[22]; if(cull==3&&area<0) return; if(cull==2&&area>0) return;
    if(area<0){ Vtx t=b; b=c; c=t; area=-area; }
    float minx=fminf(a.x,fminf(b.x,c.x)), maxx=fmaxf(a.x,fmaxf(b.x,c.x)), miny=fminf(a.y,fminf(b.y,c.y)), maxy=fmaxf(a.y,fmaxf(b.y,c.y));
    int x0=(int)floorf(minx), x1=(int)ceilf(maxx), y0=(int)floorf(miny), y1=(int)ceilf(maxy);
    int cx0=D.vpx, cy0=D.vpy, cx1=D.vpx+D.vpw, cy1=D.vpy+D.vph;
    if(cx1>rt->w)cx1=rt->w; if(cy1>rt->h)cy1=rt->h; if(cx0<0)cx0=0; if(cy0<0)cy0=0;
    if(x0<cx0)x0=cx0; if(y0<cy0)y0=cy0; if(x1>cx1)x1=cx1; if(y1>cy1)y1=cy1;
    int tl0=is_tl(b.x,b.y,c.x,c.y), tl1=is_tl(c.x,c.y,a.x,a.y), tl2=is_tl(a.x,a.y,b.x,b.y);
    float inv=1.0f/area;
    /* per-row incremental edge functions */
    float dw0=-(c.y-b.y), dw1=-(a.y-c.y), dw2=-(b.y-a.y);   /* d/dx of edge(...,px,py): -(by-ay) */
    int flat=(a.col==b.col&&b.col==c.col);
    for(int y=y0;y<y1;y++){
        float py=y+0.5f, px0=x0+0.5f;
        float w0=edge(b.x,b.y,c.x,c.y,px0,py), w1=edge(c.x,c.y,a.x,a.y,px0,py), w2=edge(a.x,a.y,b.x,b.y,px0,py);
        for(int x=x0;x<x1;x++,w0+=dw0,w1+=dw1,w2+=dw2){
            if(!(w0>0||(w0==0&&tl0))||!(w1>0||(w1==0&&tl1))||!(w2>0||(w2==0&&tl2))) continue;
            float l0=w0*inv,l1=w1*inv,l2=w2*inv;
            uint32_t col;
            if(flat) col=a.col; else {
                col=0;
                for(int ch=0;ch<4;ch++){ float v=l0*((a.col>>(ch*8))&255)+l1*((b.col>>(ch*8))&255)+l2*((c.col>>(ch*8))&255); int iv=(int)(v+0.5f); if(iv>255)iv=255; if(iv<0)iv=0; col|=(uint32_t)iv<<(ch*8); }
            }
            shade_px(x,y,col,l0*a.u+l1*b.u+l2*c.u,l0*a.v+l1*b.v+l2*c.v);
        }
    }
}
static void mulvec(const float*m,float x,float y,float z,float*o){
    for(int i=0;i<4;i++) o[i]=x*m[i]+y*m[4+i]+z*m[8+i]+m[12+i];
}
static void mat_mul(const float*a,const float*b,float*r){ float t[16]; for(int i=0;i<4;i++)for(int j=0;j<4;j++){ t[i*4+j]=0; for(int k=0;k<4;k++) t[i*4+j]+=a[i*4+k]*b[k*4+j]; } memcpy(r,t,64); }

static int fvf_stride(uint32_t fvf){
    int s=0; if(fvf&4) s+=16; else if(fvf&2) s+=12; if(fvf&0x10) s+=12; if(fvf&0x20) s+=4; if(fvf&0x40) s+=4; if(fvf&0x80) s+=4; s+=((fvf>>8)&15)*8; return s;
}
static void read_vtx(uint32_t p,uint32_t fvf,Vtx *v){
    memset(v,0,sizeof *v); v->col=0xFFFFFFFFu; uint32_t o=p;
    float x=*(float*)(gmem+o),y=*(float*)(gmem+o+4),z=*(float*)(gmem+o+8);
    if(fvf&4){ v->x=x; v->y=y; v->z=z; v->rhw=*(float*)(gmem+o+12); o+=16; }
    else { float m[16],t[16]; mat_mul(D.world,D.view,m); mat_mul(m,D.proj,t); float c[4]; mulvec(t,x,y,z,c);
        float w=c[3]!=0?c[3]:1; v->x=D.vpx+(1+c[0]/w)*D.vpw*0.5f; v->y=D.vpy+(1-c[1]/w)*D.vph*0.5f; v->z=c[2]/w; v->rhw=1/w; o+=12; }
    if(fvf&0x10) o+=12; if(fvf&0x20) o+=4;
    if(fvf&0x40){ v->col=*(uint32_t*)(gmem+o); o+=4; }
    if(fvf&0x80) o+=4;
    if(((fvf>>8)&15)>=1){ v->u=*(float*)(gmem+o); v->v=*(float*)(gmem+o+4); }
}
static void draw(uint32_t type,uint32_t fvf,uint32_t vp,uint32_t nv,uint32_t idxp,uint32_t ni){
    Surf *rt=surf_of(D.target); if(!rt) return;
    int stride=fvf_stride(fvf); D.draws++; rc_setup(rt);
    uint32_t n = idxp?ni:nv;
    Vtx *vv=malloc(sizeof(Vtx)*(nv?nv:1));
    for(uint32_t i=0;i<nv;i++) read_vtx(vp+i*stride,fvf,&vv[i]);
    #define VI(k) vv[ idxp ? rd16(idxp+2*(k)) : (k) ]
    if(type==4) for(uint32_t i=0;i+2<n;i+=3) raster_tri(rt,VI(i),VI(i+1),VI(i+2));
    else if(type==5) for(uint32_t i=0;i+2<n;i++){ if(i&1) raster_tri(rt,VI(i+1),VI(i),VI(i+2)); else raster_tri(rt,VI(i),VI(i+1),VI(i+2)); }
    else if(type==6) for(uint32_t i=1;i+1<n;i++) raster_tri(rt,VI(0),VI(i),VI(i+1));
    else { static int warned; if(!warned++) fprintf(stderr,"[D3D] primitive type %u not rasterized\n",type); }
    free(vv);
}

/* ---------- COM: IDirect3D7 ---------- */
static void fill_devdesc(uint32_t p){
    memset(gmem+p,0,236);
    wr32(p,0x1|0x10|0x20|0x40|0x80|0x100|0x200|0x400|0x800|0x1000|0x2000|0x4000|0x8000|0x80000);
    for(int k=0;k<2;k++){ uint32_t c=p+4+56*k; wr32(c,56); wr32(c+4,0x7F); wr32(c+8,0x1|0x10|0x20|0x80|0x100|0x8000); wr32(c+12,0xFF); wr32(c+16,0x7FF); wr32(c+20,0x7FF); wr32(c+24,0xFF); wr32(c+28,0xFFFF); wr32(c+32,0x1|0x4|0x8); wr32(c+36,0x3F3F); wr32(c+40,0xFF); wr32(c+44,0xF); }
    wr32(p+116,0x500); wr32(p+120,0x700); wr32(p+124,1); wr32(p+128,1); wr32(p+132,4096); wr32(p+136,4096); wr32(p+140,8192); wr32(p+144,4096); wr32(p+148,8);
    wr32(p+176,8); wr32(p+180,0x00FFFFFF); wr16(p+184,8); wr16(p+186,8); wr32(p+188,8);
    static const uint8_t hal[16]={0xE0,0x3D,0xE6,0x84,0xAA,0x46,0xCF,0x11,0x81,0x6F,0x00,0x00,0xC0,0x20,0x15,0x6E};
    memcpy(gmem+p+196,hal,16);
}
static void d3d_EnumDevices(void){
    uint32_t cb=A(1), ctx=A(2);
    uint32_t desc=ga_alloc(236,1); fill_devdesc(desc);
    uint32_t nm=ga_str("Direct3D HAL"), ds=ga_str("Direct3D Hardware Acceleration");
    uint32_t a[4]={ds,nm,desc,ctx}; guest_call(cb,4,a);
    RET(0);
}
static void d3d_CreateDevice(void){
    D.target=A(2); dev_defaults();
    Surf *t=surf_of(D.target); D.vpx=0; D.vpy=0; D.vpw=t?t->w:640; D.vph=t?t->h:480;
    if(!dev_obj){ dev_obj=ga_alloc(8,1); wr32(dev_obj,vt_dev); }
    fprintf(stderr,"[D3D] CreateDevice target=%08x %dx%d bpp=%d\n",D.target,D.vpw,D.vph,t?t->bpp:0);
    wr32(A(3),dev_obj); RET(0);
}
static void d3d_EnumZ(void){
    uint32_t cb=A(2), ctx=A(3); uint32_t pf=ga_alloc(32,1);
    wr32(pf,32); wr32(pf+4,0x400); wr32(pf+12,16); wr32(pf+20,0xFFFF);
    uint32_t a[2]={pf,ctx}; guest_call(cb,2,a); RET(0);
}
static void d3d_CreateVB(void){
    uint32_t d=A(1); uint32_t fvf=rd32(d+8), n=rd32(d+12);
    uint32_t o=ga_alloc(16,1); uint32_t buf=ga_alloc(n*fvf_stride(fvf)+16,1);
    wr32(o,vt_vb); wr32(o+4,buf); wr32(o+8,fvf); wr32(o+12,n); wr32(A(2),o); RET(0);
}
static void vb_Lock(void){ uint32_t o=A(0); wr32(A(2),rd32(o+4)); if(A(3)) wr32(A(3),rd32(o+12)*fvf_stride(rd32(o+8))); RET(0); }
static void vb_ok(void){ RET(0); }

/* ---------- COM: IDirect3DDevice7 ---------- */
static void dv_GetCaps(void){ fill_devdesc(A(1)); RET(0); }
static void dv_EnumTextureFormats(void){
    uint32_t cb=A(1), ctx=A(2); uint32_t pf=ga_alloc(32,1);
    struct { uint32_t fl,bits,r,g,b,a; } f[]={
        {0x41,16,0x0F00,0x00F0,0x000F,0xF000},
        {0x40,16,0xF800,0x07E0,0x001F,0},
        {0x41,16,0x7C00,0x03E0,0x001F,0x8000},
        {0x41,32,0xFF0000,0x00FF00,0x0000FF,0xFF000000u},
    };
    for(unsigned i=0;i<sizeof f/sizeof *f;i++){
        memset(gmem+pf,0,32); wr32(pf,32); wr32(pf+4,f[i].fl); wr32(pf+12,f[i].bits); wr32(pf+16,f[i].r); wr32(pf+20,f[i].g); wr32(pf+24,f[i].b); wr32(pf+28,f[i].a);
        uint32_t a[2]={pf,ctx}; if(guest_call(cb,2,a)==0) break;
    }
    RET(0);
}
static void dv_SetRenderTarget(void){ D.target=A(1); RET(0); }
static void dv_GetRenderTarget(void){ wr32(A(1),D.target); RET(0); }
static void dv_Clear(void){
    uint32_t cnt=A(1), rects=A(2), flags=A(3), color=A(4); Surf *rt=surf_of(D.target);
    if((flags&1)&&rt){
        uint32_t px=from_argb(rt,color);
        if(!cnt){ for(int y=D.vpy;y<D.vpy+D.vph&&y<rt->h;y++) for(int x=D.vpx;x<D.vpx+D.vpw&&x<rt->w;x++) put_raw(rt,x,y,px); }
        else for(uint32_t i=0;i<cnt;i++){ int l=rd32(rects+16*i),t=rd32(rects+16*i+4),r=rd32(rects+16*i+8),b=rd32(rects+16*i+12);
            for(int y=t;y<b&&y<rt->h;y++) for(int x=l;x<r&&x<rt->w;x++) if(x>=0&&y>=0) put_raw(rt,x,y,px); }
    }
    RET(0);
}
static float *xform_of(uint32_t st){ return st==256?D.world:st==2?D.view:st==3?D.proj:NULL; }
static void dv_SetTransform(void){ float *m=xform_of(A(1)); if(m) memcpy(m,gmem+A(2),64); RET(0); }
static void dv_GetTransform(void){ float *m=xform_of(A(1)); if(m) memcpy(gmem+A(2),m,64); RET(0); }
static void dv_SetViewport(void){ uint32_t p=A(1); D.vpx=rd32(p); D.vpy=rd32(p+4); D.vpw=rd32(p+8); D.vph=rd32(p+12); RET(0); }
static void dv_GetViewport(void){ uint32_t p=A(1); wr32(p,D.vpx); wr32(p+4,D.vpy); wr32(p+8,D.vpw); wr32(p+12,D.vph); RET(0); }
static void dv_SetRenderState(void){ if(A(1)<256) D.rs[A(1)]=A(2); RET(0); }
static void dv_GetRenderState(void){ wr32(A(2),A(1)<256?D.rs[A(1)]:0); RET(0); }
static void dv_DrawPrimitive(void){ draw(A(1),A(2),A(3),A(4),0,0); RET(0); }
static void dv_DrawIndexedPrimitive(void){ draw(A(1),A(2),A(3),A(4),A(5),A(6)); RET(0); }
static void dv_DrawPrimitiveVB(void){ uint32_t vb=A(2); uint32_t fvf=rd32(vb+8); draw(A(1),fvf,rd32(vb+4)+A(3)*fvf_stride(fvf),A(4),0,0); RET(0); }
static void dv_DrawIndexedPrimitiveVB(void){ uint32_t vb=A(2); uint32_t fvf=rd32(vb+8); draw(A(1),fvf,rd32(vb+4),rd32(vb+12),A(5),A(6)); RET(0); }
static void dv_SetTexture(void){ if(A(1)<8) D.tex[A(1)]=A(2); RET(0); }
static void dv_GetTexture(void){ wr32(A(2),A(1)<8?D.tex[A(1)]:0); RET(0); }
static void dv_SetTSS(void){ if(A(1)<8&&A(2)<32) D.tss[A(1)][A(2)]=A(3); RET(0); }
static void dv_GetTSS(void){ wr32(A(3),(A(1)<8&&A(2)<32)?D.tss[A(1)][A(2)]:0); RET(0); }
static void dv_ValidateDevice(void){ if(A(1)) wr32(A(1),1); RET(0); }
static void dv_Load(void){ /* (dst,dstpt,src,srcrect,flags) : copy whole surface */
    Surf *d=surf_of(A(1)), *s=surf_of(A(3)); if(d) d->cache_ok=0; if(d&&s&&d->bpp==s->bpp){ int h=s->h<d->h?s->h:d->h; int rowb=(s->w<d->w?s->w:d->w)*(s->bpp/8); for(int y=0;y<h;y++) memcpy(gmem+d->pix+y*d->pitch,gmem+s->pix+y*s->pitch,rowb); } RET(0); }

static void warn_default(void){ static unsigned char seen[512]; int i=g_cur_idx&511; if(!seen[i]){ seen[i]=1; fprintf(stderr,"[D3D stub] %s -> S_OK\n",hle_name(g_cur_idx)); } RET(0); }

enum { T_QI,T_AddRef,T_Release,T_EnumDevices,T_CreateDevice,T_CreateVertexBuffer,T_EnumZBufferFormats,T_EvictManagedTextures,T_N };
static const MDef t_m[T_N]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"EnumDevices",3},{"CreateDevice",4},{"CreateVertexBuffer",4},{"EnumZBufferFormats",4},{"EvictManagedTextures",1}};
#define DVN 49
static const MDef dv_m[DVN]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetCaps",2},{"EnumTextureFormats",3},{"BeginScene",1},{"EndScene",1},{"GetDirect3D",2},{"SetRenderTarget",3},{"GetRenderTarget",2},{"Clear",7},{"SetTransform",3},{"GetTransform",3},{"SetViewport",2},{"MultiplyTransform",3},{"GetViewport",2},{"SetMaterial",2},{"GetMaterial",2},{"SetLight",3},{"GetLight",3},{"SetRenderState",3},{"GetRenderState",3},{"BeginStateBlock",1},{"EndStateBlock",2},{"PreLoad",2},{"DrawPrimitive",6},{"DrawIndexedPrimitive",8},{"SetClipStatus",2},{"GetClipStatus",2},{"DrawPrimitiveStrided",6},{"DrawIndexedPrimitiveStrided",8},{"DrawPrimitiveVB",6},{"DrawIndexedPrimitiveVB",8},{"ComputeSphereVisibility",6},{"GetTexture",3},{"SetTexture",3},{"GetTextureStageState",4},{"SetTextureStageState",4},{"ValidateDevice",2},{"ApplyStateBlock",2},{"CaptureStateBlock",2},{"DeleteStateBlock",2},{"CreateStateBlock",3},{"Load",6},{"LightEnable",3},{"GetLightEnable",3},{"SetClipPlane",3},{"GetClipPlane",3},{"GetInfo",4}};
#define VBN 9
static const MDef vb_m[VBN]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"Lock",4},{"Unlock",1},{"ProcessVertices",8},{"GetVertexBufferDesc",2},{"Optimize",3},{"ProcessVerticesStrided",8}};
static hle_fn t_impl[T_N], dv_impl[DVN], vb_impl[VBN];

uint32_t d3d_get_object(void){ if(!d3d_obj){ d3d_obj=ga_alloc(8,1); wr32(d3d_obj,vt_d3d);} return d3d_obj; }
void d3d_register(void){
    for(int i=0;i<T_N;i++) t_impl[i]=warn_default; for(int i=0;i<DVN;i++) dv_impl[i]=warn_default; for(int i=0;i<VBN;i++) vb_impl[i]=warn_default;
    t_impl[0]=dv_impl[0]=vb_impl[0]=com_default; /* QI replaced below */
    t_impl[1]=dv_impl[1]=vb_impl[1]=com_addref; t_impl[2]=dv_impl[2]=vb_impl[2]=com_release;
    t_impl[0]=dv_impl[0]=vb_impl[0]=com_addref; /* QI on these objects: return self */
    t_impl[T_EnumDevices]=d3d_EnumDevices; t_impl[T_CreateDevice]=d3d_CreateDevice; t_impl[T_EnumZBufferFormats]=d3d_EnumZ; t_impl[T_CreateVertexBuffer]=d3d_CreateVB;
    dv_impl[3]=dv_GetCaps; dv_impl[4]=dv_EnumTextureFormats; dv_impl[5]=vb_ok; dv_impl[6]=vb_ok; dv_impl[8]=dv_SetRenderTarget; dv_impl[9]=dv_GetRenderTarget;
    dv_impl[10]=dv_Clear; dv_impl[11]=dv_SetTransform; dv_impl[12]=dv_GetTransform; dv_impl[13]=dv_SetViewport; dv_impl[15]=dv_GetViewport;
    dv_impl[20]=dv_SetRenderState; dv_impl[21]=dv_GetRenderState; dv_impl[25]=dv_DrawPrimitive; dv_impl[26]=dv_DrawIndexedPrimitive;
    dv_impl[31]=dv_DrawPrimitiveVB; dv_impl[32]=dv_DrawIndexedPrimitiveVB; dv_impl[34]=dv_GetTexture; dv_impl[35]=dv_SetTexture;
    dv_impl[36]=dv_GetTSS; dv_impl[37]=dv_SetTSS; dv_impl[38]=dv_ValidateDevice; dv_impl[43]=dv_Load;
    vb_impl[3]=vb_Lock; vb_impl[4]=vb_ok;
    static Iface t={"IDirect3D7",T_N,t_m,t_impl}, dv={"IDirect3DDevice7",DVN,dv_m,dv_impl}, vb={"IDirect3DVertexBuffer7",VBN,vb_m,vb_impl};
    vt_d3d=make_vtable(&t); vt_dev=make_vtable(&dv); vt_vb=make_vtable(&vb);
}
