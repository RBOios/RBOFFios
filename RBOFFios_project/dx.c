/* DirectDraw 7 / DirectInput 8 / DirectSound HLE (headless reference implementation) */
#include "dxshared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>


uint32_t make_vtable(Iface *ifc){
    uint32_t vt=ga_alloc(4*ifc->n,1);
    for(int i=0;i<ifc->n;i++){
        char nm[96]; snprintf(nm,sizeof nm,"%s::%s",ifc->iface,ifc->m[i].name);
        int idx=hle_register("COM",nm,ifc->m[i].nargs,ifc->impl[i]);
        wr32(vt+4*i,hle_thunk(idx));
    }
    if(getenv("RBO_VT")) fprintf(stderr,"[vtable] %s at %08x (%d entries)\n",ifc->iface,vt,ifc->n);
    return vt;
}
extern int g_cur_idx;
static uint32_t vt_dsb, vt_notify;
void com_default(void){
    fprintf(stderr,"[COM stub] %s -> S_OK\n",hle_name(g_cur_idx));
    RET(0);
}
void com_addref(void){ RET(1); }
void com_release(void){ RET(0); }
static void com_qi(void){
    /* only IUnknown and the interfaces we actually implement succeed */
    uint32_t d1=A(1)?rd32(A(1)):0xFFFFFFFFu;
    if(d1==0xb0210783u&&vt_dsb&&rd32(A(0))==vt_dsb){ uint32_t o=ga_alloc(8,1); wr32(o,vt_notify); wr32(o+4,rd32(A(0)+4)); if(A(2)) wr32(A(2),o); RET(0); return; }
    if(d1==0xf5049e77u){ if(A(2)) wr32(A(2),d3d_get_object()); RET(0); return; }
    static const uint32_t ok[]={0x00000000u,0x15e65ec0u,0x06675a80u,0xBF798030u,0x54D41080u,0xC50A7E93u,0x279AFA85u};
    for(unsigned i=0;i<sizeof ok/sizeof *ok;i++) if(d1==ok[i]){ if(A(2)) wr32(A(2),A(0)); RET(0); return; }
    fprintf(stderr,"[COM] QueryInterface refused iid %08x (%s)\n",d1,hle_name(g_cur_idx));
    if(A(2)) wr32(A(2),0); RET(0x80004002u);
}

#define PF_FLAGS 76
/* ========================= DirectDraw ========================= */

#define MAXS 512
static Surf S[MAXS];
static int disp_w=640, disp_h=480, disp_bpp=16, disp_set=0;
static int pr_l=0,pr_t=0,pr_r=0,pr_b=0;   /* last Blt-to-primary rect (windowed present region) */
static int primary_id=-1;
static uint32_t vt_dd=0, vt_surf=0;
static uint32_t dd_obj=0;
static int g_dump_every=0;

static void fmt_for_bpp(int bpp,uint32_t*r,uint32_t*g,uint32_t*b,uint32_t*a){
    *a=0;
    if(bpp==16){*r=0xF800;*g=0x07E0;*b=0x001F;}
    else {*r=0xFF0000;*g=0x00FF00;*b=0x0000FF; if(bpp==32)*a=0;}
}
static int new_surface(int w,int h,int bpp,uint32_t r,uint32_t g,uint32_t b,uint32_t a,uint32_t *objout){
    int id=-1; for(int i=0;i<MAXS;i++) if(!S[i].used){id=i;break;}
    if(id<0){ fprintf(stderr,"[DDraw] out of surfaces\n"); abort(); }
    Surf *s=&S[id]; gpu_surf_release(s); memset(s,0,sizeof *s); s->id=id; s->used=1; s->w=w; s->h=h; s->bpp=bpp;
    s->pitch=((w*(bpp/8))+3)&~3; s->pix=ga_alloc(s->pitch*h+16,1);
    s->rmask=r; s->gmask=g; s->bmask=b; s->amask=a; s->back_id=-1;
    s->obj=ga_alloc(8,1); wr32(s->obj,vt_surf); wr32(s->obj+4,id);
    if(objout)*objout=s->obj;
    return id;
}
Surf *surf_of(uint32_t obj){ if(!obj) return NULL; return &S[rd32(obj+4)]; }
static uint64_t last_vb=0;
static uint64_t nowus(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+t.tv_nsec/1000; }

static uint32_t getpx(Surf*s,int x,int y){
    uint8_t *p=gmem+s->pix+y*s->pitch+x*(s->bpp/8);
    if(s->bpp==16) return *(uint16_t*)p; if(s->bpp==32) return *(uint32_t*)p;
    if(s->bpp==24) return p[0]|(p[1]<<8)|(p[2]<<16); return *p;
}
static void putpx(Surf*s,int x,int y,uint32_t v){
    uint8_t *p=gmem+s->pix+y*s->pitch+x*(s->bpp/8);
    if(s->bpp==16) *(uint16_t*)p=v; else if(s->bpp==32) *(uint32_t*)p=v; else if(s->bpp==24){p[0]=v;p[1]=v>>8;p[2]=v>>16;} else *p=v;
}
static int mask_shift(uint32_t m){ int s=0; if(!m) return 0; while(!(m&1)){m>>=1;s++;} return s; }
static int mask_bits(uint32_t m){ int n=0; m>>=mask_shift(m); while(m&1){n++;m>>=1;} return n; }
static uint32_t convert(Surf*from,Surf*to,uint32_t v){
    if(from->bpp==to->bpp&&from->rmask==to->rmask&&from->gmask==to->gmask&&from->bmask==to->bmask) return v;
    uint32_t r=(v&from->rmask)>>mask_shift(from->rmask), g=(v&from->gmask)>>mask_shift(from->gmask), b=(v&from->bmask)>>mask_shift(from->bmask);
    int rb=mask_bits(from->rmask),gb=mask_bits(from->gmask),bb=mask_bits(from->bmask);
    r=rb?(r*255/((1<<rb)-1)):0; g=gb?(g*255/((1<<gb)-1)):0; b=bb?(b*255/((1<<bb)-1)):0;
    int tr=mask_bits(to->rmask),tg=mask_bits(to->gmask),tb=mask_bits(to->bmask);
    return ((r>>(8-tr))<<mask_shift(to->rmask))|((g>>(8-tg))<<mask_shift(to->gmask))|((b>>(8-tb))<<mask_shift(to->bmask));
}
typedef struct { int l,t,r,b; } Rc;
static Rc rect_of(uint32_t p,Surf*s){ Rc r={0,0,s->w,s->h}; if(p){ r.l=(int)rd32(p); r.t=(int)rd32(p+4); r.r=(int)rd32(p+8); r.b=(int)rd32(p+12);} return r; }


/* Fast stretch/convert blit for 16/32 bpp. Per-pixel divisions and the generic convert() made large Blts cost milliseconds; this uses a per-column source table,
   a 64K lookup table for 16bpp sources, and direct shifts for 32bpp sources. Same results as the generic loop. Returns 0 when it does not apply. */
static int blt_fast(Surf*d,Rc dr,Surf*s,Rc sr,int keyed){
    if((s->bpp!=16&&s->bpp!=32)||(d->bpp!=16&&d->bpp!=32)) return 0;
    int dw=dr.r-dr.l, dh=dr.b-dr.t, sw=sr.r-sr.l, sh=sr.b-sr.t;
    int same=(d->bpp==s->bpp&&d->rmask==s->rmask&&d->gmask==s->gmask&&d->bmask==s->bmask);
    int mode;   /* 0 raw copy, 1 16bpp LUT, 2 32bpp channel shifts */
    static uint32_t *lut=NULL; static uint32_t lk[7];
    int srs=0,sgs=0,sbs=0,drs=0,dgs=0,dbs=0,drn=0,dgn=0,dbn=0;
    if(same) mode=0;
    else if(s->bpp==16){
        uint32_t key[7]={s->rmask,s->gmask,s->bmask,d->rmask,d->gmask,d->bmask,(uint32_t)d->bpp};
        if(!lut){ lut=malloc(65536*sizeof(uint32_t)); if(!lut) return 0; memset(lk,0xff,sizeof lk); }
        if(memcmp(lk,key,sizeof key)){ for(uint32_t v=0;v<65536;v++) lut[v]=convert(s,d,v); memcpy(lk,key,sizeof lk); }
        mode=1;
    } else {
        if(mask_bits(s->rmask)!=8||mask_bits(s->gmask)!=8||mask_bits(s->bmask)!=8) return 0;
        srs=mask_shift(s->rmask); sgs=mask_shift(s->gmask); sbs=mask_shift(s->bmask);
        drs=mask_shift(d->rmask); dgs=mask_shift(d->gmask); dbs=mask_shift(d->bmask);
        drn=8-mask_bits(d->rmask); dgn=8-mask_bits(d->gmask); dbn=8-mask_bits(d->bmask);
        mode=2;
    }
    static int *sxm=NULL; static int cap=0;
    if(dw>cap){ int *n=realloc(sxm,(size_t)dw*sizeof(int)); if(!n) return 0; sxm=n; cap=dw; }
    int xa=0, xb=dw; if(dr.l<0) xa=-dr.l; if(dr.l+dw>d->w) xb=d->w-dr.l;
    if(xa>=xb) return 1;
    for(int x=xa;x<xb;x++){ int sx=sr.l+(int)((int64_t)x*sw/dw); sxm[x]=(sx<0||sx>=s->w)?-1:sx; }
    const int ckon=keyed&&s->ck_valid; const uint32_t klo=s->ck_lo, khi=s->ck_hi; const int sb=s->bpp/8, db=d->bpp/8;
    for(int y=0;y<dh;y++){
        int dy=dr.t+y; if(dy<0||dy>=d->h) continue;
        int sy=sr.t+(int)((int64_t)y*sh/dh); if(sy<0||sy>=s->h) continue;
        const uint8_t *sp=gmem+s->pix+(size_t)sy*s->pitch; uint8_t *dp=gmem+d->pix+(size_t)dy*d->pitch+(size_t)(dr.l+xa)*db;
        for(int x=xa;x<xb;x++,dp+=db){
            int sx=sxm[x]; if(sx<0) continue;
            uint32_t v=(sb==2)?*(const uint16_t*)(sp+sx*2):*(const uint32_t*)(sp+sx*4);
            if(ckon&&v>=klo&&v<=khi) continue;
            if(mode==1) v=lut[v&0xffff];
            else if(mode==2) v=((((v>>srs)&255)>>drn)<<drs)|((((v>>sgs)&255)>>dgn)<<dgs)|((((v>>sbs)&255)>>dbn)<<dbs);
            if(db==2) *(uint16_t*)dp=(uint16_t)v; else *(uint32_t*)dp=v;
        }
    }
    return 1;
}
static void do_blt(Surf*d,Rc dr,Surf*s,Rc sr,int keyed){
    int dw=dr.r-dr.l, dh=dr.b-dr.t, sw=sr.r-sr.l, sh=sr.b-sr.t;
    if(dw<=0||dh<=0||sw<=0||sh<=0) return;
    int same=(d->bpp==s->bpp&&d->rmask==s->rmask&&d->gmask==s->gmask&&d->bmask==s->bmask);
    /* fast path: identical format, 1:1, no colour key, fully inside both surfaces -> copy whole rows */
    if(same && d!=s && !(keyed&&s->ck_valid) && dw==sw && dh==sh && (d->bpp==16||d->bpp==32)
       && dr.l>=0&&dr.t>=0&&dr.r<=d->w&&dr.b<=d->h && sr.l>=0&&sr.t>=0&&sr.r<=s->w&&sr.b<=s->h){
        int bp=d->bpp/8;
        for(int y=0;y<dh;y++) memcpy(gmem+d->pix+(size_t)(dr.t+y)*d->pitch+dr.l*bp, gmem+s->pix+(size_t)(sr.t+y)*s->pitch+sr.l*bp, (size_t)dw*bp);
        return;
    }
    if(blt_fast(d,dr,s,sr,keyed)) return;
    for(int y=0;y<dh;y++){
        int dy=dr.t+y; if(dy<0||dy>=d->h) continue;
        int sy=sr.t+(int)((int64_t)y*sh/dh); if(sy<0||sy>=s->h) continue;
        for(int x=0;x<dw;x++){
            int dx=dr.l+x; if(dx<0||dx>=d->w) continue;
            int sx=sr.l+(int)((int64_t)x*sw/dw); if(sx<0||sx>=s->w) continue;
            uint32_t v=getpx(s,sx,sy);
            if(keyed&&s->ck_valid&&v>=s->ck_lo&&v<=s->ck_hi) continue;
            putpx(d,dx,dy,same?v:convert(s,d,v));
        }
    }
}
static void do_fill(Surf*d,Rc r,uint32_t c){
    if(d->bpp==16||d->bpp==32){
        int l=r.l<0?0:r.l, t=r.t<0?0:r.t, rr=r.r>d->w?d->w:r.r, bb=r.b>d->h?d->h:r.b;
        if(l>=rr||t>=bb) return;
        for(int y=t;y<bb;y++){
            uint8_t *row=gmem+d->pix+(size_t)y*d->pitch;
            if(d->bpp==16){ uint16_t *p=(uint16_t*)row+l; uint16_t v=(uint16_t)c; for(int x=l;x<rr;x++) *p++=v; }
            else { uint32_t *p=(uint32_t*)row+l; for(int x=l;x<rr;x++) *p++=c; }
        }
        return;
    }
    for(int y=r.t;y<r.b;y++){ if(y<0||y>=d->h)continue; for(int x=r.l;x<r.r;x++){ if(x<0||x>=d->w)continue; putpx(d,x,y,c);} }
}

void dx_set_key(int dik,int down);
static struct { int frame,dik,down; } kscript[256]; static int nks=0, ksinit=0;
static void run_keyscript(void){
    if(!ksinit){ ksinit=1; const char *e=getenv("RBO_KEYS"); if(e){ char *c=strdup(e); for(char *t=strtok(c,",");t&&nks<256;t=strtok(NULL,",")){ int f,d,dn; if(sscanf(t,"%d:%x:%d",&f,&d,&dn)==3){ kscript[nks].frame=f; kscript[nks].dik=d; kscript[nks].down=dn; nks++; } } } }
    for(int i=0;i<nks;i++) if(kscript[i].frame==g_frames) dx_set_key(kscript[i].dik,kscript[i].down);
}
void ctl_frame_hook(void);
void dx_script_add(int rel,int dik,int down){ if(nks<256){ kscript[nks].frame=g_frames+rel; kscript[nks].dik=dik; kscript[nks].down=down; nks++; } ksinit=1; }
void dx_script_clear(void){ nks=0; }
void dx_set_dump_every(int n){ g_dump_every=n; }
void present_frame_file(const char *path,const uint8_t *pix,int w,int h,int pitch,int bpp,uint32_t rm,uint32_t gm,uint32_t bm);
void dx_dump_primary(const char *path){ if(primary_id<0) return; Surf *p=&S[primary_id]; gpu_surf_access(p,0); present_frame_file(path,gmem+p->pix,p->w,p->h,p->pitch,p->bpp,p->rmask,p->gmask,p->bmask); }
void (*g_host_present)(void *ctx,const uint32_t *bgra,int w,int h)=NULL; void *g_host_ctx=NULL; volatile int g_stop_req=0;
static uint32_t *host_buf=NULL; static size_t host_cap=0;
static uint32_t hp_lut16[65536]; static uint32_t hp_lut_rm=0,hp_lut_gm=0,hp_lut_bm=0; static int hp_lut_ok=0;
uint64_t g_stat_present_us=0;      /* total time spent converting + handing frames to the host (for the on-screen stats) */
static void host_present(Surf *p,int l,int t,int r,int b){
    int w=r-l, h=b-t; if(w<=0||h<=0) return;
    uint64_t t_start=nowus();
    if((size_t)w*h>host_cap){ free(host_buf); host_cap=(size_t)w*h; host_buf=malloc(host_cap*4); }
    uint32_t rm=p->rmask,gm=p->gmask,bm=p->bmask; int rs=mask_shift(rm),gs=mask_shift(gm),bs=mask_shift(bm), rb=mask_bits(rm),gb=mask_bits(gm),bb=mask_bits(bm);
    if(p->bpp==16){
        /* 64K-entry table built once per pixel format (same maths as before, just precomputed) */
        if(!hp_lut_ok||hp_lut_rm!=rm||hp_lut_gm!=gm||hp_lut_bm!=bm){
            for(uint32_t v=0;v<65536;v++){
                uint32_t R=rb?(((v&rm)>>rs)*255/((1u<<rb)-1)):0, G=gb?(((v&gm)>>gs)*255/((1u<<gb)-1)):0, B=bb?(((v&bm)>>bs)*255/((1u<<bb)-1)):0;
                hp_lut16[v]=0xFF000000u|(R<<16)|(G<<8)|B;
            }
            hp_lut_rm=rm; hp_lut_gm=gm; hp_lut_bm=bm; hp_lut_ok=1;
        }
        for(int y=0;y<h;y++){
            const uint16_t *row=(const uint16_t*)(gmem+p->pix+(size_t)(t+y)*p->pitch+l*2); uint32_t *o=host_buf+(size_t)y*w;
            for(int x=0;x<w;x++) o[x]=hp_lut16[row[x]];
        }
    } else if(rb==8&&gb==8&&bb==8){
        for(int y=0;y<h;y++){
            const uint32_t *row=(const uint32_t*)(gmem+p->pix+(size_t)(t+y)*p->pitch+l*4); uint32_t *o=host_buf+(size_t)y*w;
            for(int x=0;x<w;x++){ uint32_t v=row[x]; o[x]=0xFF000000u|(((v&rm)>>rs)<<16)|(((v&gm)>>gs)<<8)|((v&bm)>>bs); }
        }
    } else {
        for(int y=0;y<h;y++){
            const uint8_t *row=gmem+p->pix+(size_t)(t+y)*p->pitch+l*(p->bpp/8); uint32_t *o=host_buf+(size_t)y*w;
            for(int x=0;x<w;x++){
                uint32_t v=((const uint32_t*)row)[x];
                uint32_t R=rb?(((v&rm)>>rs)*255/((1u<<rb)-1)):0, G=gb?(((v&gm)>>gs)*255/((1u<<gb)-1)):0, B=bb?(((v&bm)>>bs)*255/((1u<<bb)-1)):0;
                o[x]=0xFF000000u|(R<<16)|(G<<8)|B;
            }
        }
    }
    g_host_present(g_host_ctx,host_buf,w,h);
    g_stat_present_us+=nowus()-t_start;
}
void ds_tick(void);
static void present(void){
    if(primary_id<0) return;
    Surf *p=&S[primary_id]; gpu_surf_access(p,0);
    g_frames++; hle_vsync_advance(); ds_tick(); sched_frame_boundary(); run_keyscript(); ctl_frame_hook();
    if(g_stop_req){ cpu.halted=1; return; }
    if(g_host_present && (p->bpp==16||p->bpp==32)){
        if(!disp_set && pr_r>pr_l && pr_b>pr_t){ int l=pr_l<0?0:pr_l,t=pr_t<0?0:pr_t,r=pr_r>p->w?p->w:pr_r,b=pr_b>p->h?p->h:pr_b; host_present(p,l,t,r,b); }
        else host_present(p,0,0,p->w,p->h);
    }
    if(g_dump_every && g_frames%g_dump_every==0){
        if(!disp_set && pr_r>pr_l && pr_b>pr_t){
            int l=pr_l<0?0:pr_l, t=pr_t<0?0:pr_t, r=pr_r>p->w?p->w:pr_r, b=pr_b>p->h?p->h:pr_b;
            present_frame(gmem+p->pix+t*p->pitch+l*(p->bpp/8),r-l,b-t,p->pitch,p->bpp,p->rmask,p->gmask,p->bmask);
        } else present_frame(gmem+p->pix,p->w,p->h,p->pitch,p->bpp,p->rmask,p->gmask,p->bmask);
    }
    if(g_max_frames && g_frames>=g_max_frames){ fprintf(stderr,"[present] reached %d frames, stopping\n",g_frames); cpu.halted=1; }
}
static void fill_pf(uint32_t p,Surf*s){
    wr32(p,32); wr32(p+4,s->bpp==8?0x20|0x4:0x40); wr32(p+8,0); wr32(p+12,s->bpp);
    wr32(p+16,s->rmask); wr32(p+20,s->gmask); wr32(p+24,s->bmask); wr32(p+28,s->amask);
}
static void fill_desc(uint32_t d,Surf*s,int lock){
    uint32_t sz=rd32(d); if(sz!=124&&sz!=108) sz=124;
    memset(gmem+d,0,sz); wr32(d,sz);
    wr32(d+4,0x1|0x2|0x4|0x8|0x1000|(lock?0x0:0)); wr32(d+8,s->h); wr32(d+12,s->w); wr32(d+16,s->pitch);
    if(lock) wr32(d+36,s->pix);
    fill_pf(d+72,s);
    wr32(d+104,s->is_primary?0x200:0x40);
}

static void dd_CreateSurface(void){
    uint32_t d=A(1), out=A(2);
    uint32_t flags=rd32(d+4), caps=rd32(d+104);
    int w=rd32(d+12), h=rd32(d+8);
    uint32_t r,g,b,a; Surf tmp;
    if(caps&0x200){ if(disp_set){ w=disp_w; h=disp_h; } else { w=1920; h=1200; } }
    int bpp=disp_bpp; fmt_for_bpp(bpp,&r,&g,&b,&a);
    if(flags&0x1000){ uint32_t pf=d+72; if(rd32(pf+4)&0x40){ bpp=rd32(pf+12); r=rd32(pf+16); g=rd32(pf+20); b=rd32(pf+24); a=rd32(pf+28);} }
    (void)tmp;
    uint32_t obj; int id=new_surface(w,h,bpp,r,g,b,a,&obj);
    if(caps&0x200){
        S[id].is_primary=1; primary_id=id;
        if((flags&0x20)&&rd32(d+20)>=1){
            uint32_t bobj; int bid=new_surface(w,h,bpp,r,g,b,a,&bobj); S[bid].is_back=1; S[id].back_id=bid;
        }
    }
    if(g_trace) fprintf(stderr,"[DDraw] CreateSurface id=%d %dx%d bpp=%d caps=%08x flags=%08x\n",id,w,h,bpp,caps,flags);
    wr32(out,obj); RET(0);
}
static void dd_SetDisplayMode(void){ disp_set=1; disp_w=A(1); disp_h=A(2); disp_bpp=A(3); fprintf(stderr,"[DDraw] SetDisplayMode %dx%dx%d\n",disp_w,disp_h,disp_bpp); RET(0); }
static void dd_GetDisplayMode(void){ Surf t; memset(&t,0,sizeof t); t.w=disp_w;t.h=disp_h;t.bpp=disp_bpp; t.pitch=disp_w*disp_bpp/8; fmt_for_bpp(disp_bpp,&t.rmask,&t.gmask,&t.bmask,&t.amask); fill_desc(A(1),&t,0); RET(0); }
static void dd_SetCooperativeLevel(void){ fprintf(stderr,"[DDraw] SetCooperativeLevel flags=%08x\n",A(2)); RET(0); }
static void dd_EnumDisplayModes(void){
    uint32_t cb=A(4), ctx=A(3); int modes[][3]={{640,480,16},{640,480,32},{800,600,16},{800,600,32},{1024,768,16},{1024,768,32}};
    uint32_t d=ga_alloc(128,1);
    for(int i=0;i<(int)(sizeof modes/sizeof modes[0]);i++){
        Surf t; memset(&t,0,sizeof t); t.w=modes[i][0]; t.h=modes[i][1]; t.bpp=modes[i][2]; t.pitch=t.w*t.bpp/8; fmt_for_bpp(t.bpp,&t.rmask,&t.gmask,&t.bmask,&t.amask);
        fill_desc(d,&t,0); uint32_t a[2]={d,ctx}; uint32_t rr=guest_call(cb,2,a); if(rr==0) break;
    }
    ga_free(d); RET(0);
}
static void dd_GetCaps(void){
    if(A(1)){ uint32_t p=A(1); uint32_t sz=rd32(p); memset(gmem+p,0,sz); wr32(p,sz); wr32(p+4,0x40|0x200|0x400000|0x04000000|0x80000000u); wr32(p+12,0x8); wr32(p+60,64u<<20); wr32(p+64,60u<<20); }
    if(A(2)){ uint32_t p=A(2); uint32_t sz=rd32(p); memset(gmem+p,0,sz); wr32(p,sz); wr32(p+4,0x40|0x200|0x400000|0x04000000|0x80000000u); wr32(p+12,0x8); }
    RET(0);
}
static void dd_GetAvailableVidMem(void){ if(A(2))wr32(A(2),64u<<20); if(A(3))wr32(A(3),60u<<20); RET(0); }
static void dd_WaitForVerticalBlank(void){
    uint64_t t=nowus(); uint64_t period=16667; uint64_t next=(last_vb? last_vb+period : t);
    if(g_fast){ hle_vsync_advance(); RET(0); return; }
    if(next>t) hle_sleep_us(next-t,2); last_vb=next>t?next:t; RET(0);
}

static void sf_GetAttachedSurface(void){ Surf *s=surf_of(A(0)); if(s&&s->back_id>=0){ wr32(A(2),S[s->back_id].obj); RET(0);} else { wr32(A(2),0); RET(0x887602ACu /*DDERR_NOTFOUND*/);} }
static void sf_Flip(void){
    Surf *s=surf_of(A(0)); if(s&&s->back_id>=0){ Surf *b=&S[s->back_id]; gpu_surf_access(b,0); gpu_surf_access(s,1); memcpy(gmem+s->pix,gmem+b->pix,s->pitch*s->h); }
    present(); RET(0);
}
static void sf_Blt(void){
    Surf *d=surf_of(A(0)); d->cache_ok=0; Rc dr=rect_of(A(1),d); uint32_t flags=A(4), fx=A(5);
    Surf *s=surf_of(A(2));
    gpu_surf_access(d,1); if(s&&s!=d) gpu_surf_access(s,0);
    if(flags&0x400){ do_fill(d,dr,rd32(fx+80)); }
    else if(s){ Rc sr=rect_of(A(3),s);
        /* Native resolution: the game draws 640x480 and stretches it to the (fake) 1280x1024 window.  Copy 1:1 instead and let the GPU scale the
           frame (MetalPresenter draws it aspect-fit at the same 5:4 shape), so the CPU neither stretches nor converts 1.3M pixels per frame. */
        if(d->is_primary && !disp_set){
            int sw=sr.r-sr.l, sh=sr.b-sr.t;
            if(sw>0&&sh>0&&((dr.r-dr.l)>sw||(dr.b-dr.t)>sh)){ dr.r=dr.l+sw; dr.b=dr.t+sh; }
        }
        do_blt(d,dr,s,sr,(flags&0x8000)!=0); }
    if(d->is_primary){ if(dr.r>dr.l&&dr.b>dr.t){ pr_l=dr.l; pr_t=dr.t; pr_r=dr.r; pr_b=dr.b; } present(); }
    RET(0);
}
static void sf_BltFast(void){
    Surf *d=surf_of(A(0)); d->cache_ok=0; int x=(int)A(1), y=(int)A(2); Surf *s=surf_of(A(3)); Rc sr=rect_of(A(4),s); uint32_t tr=A(5);
    gpu_surf_access(d,1); if(s!=d) gpu_surf_access(s,0);
    Rc dr={x,y,x+(sr.r-sr.l),y+(sr.b-sr.t)};
    do_blt(d,dr,s,sr,(tr&1)!=0); if(d->is_primary) present(); RET(0);
}
static void sf_Lock(void){
    Surf *s=surf_of(A(0)); uint32_t rc=A(1), d=A(2); gpu_surf_access(s,1); fill_desc(d,s,1);
    if(rc){ Rc r=rect_of(rc,s); wr32(d+36,s->pix+r.t*s->pitch+r.l*(s->bpp/8)); }
    s->locked=1; s->cache_ok=0; RET(0);
}
static void sf_Unlock(void){ Surf *s=surf_of(A(0)); s->locked=0; s->cache_ok=0; if(s->is_primary) present(); RET(0); }
static void sf_GetSurfaceDesc(void){ fill_desc(A(1),surf_of(A(0)),0); RET(0); }
static void sf_GetPixelFormat(void){ fill_pf(A(1),surf_of(A(0))); RET(0); }
static void sf_SetColorKey(void){
    Surf *s=surf_of(A(0)); s->cache_ok=0; uint32_t f=A(1), k=A(2);
    if(f&0x8){ if(k){ s->ck_valid=1; s->ck_lo=rd32(k); s->ck_hi=rd32(k+4);} else s->ck_valid=0; }
    RET(0);
}
static void sf_IsLost(void){ RET(0); }

enum { DD_QI,DD_AddRef,DD_Release,DD_Compact,DD_CreateClipper,DD_CreatePalette,DD_CreateSurface,DD_DuplicateSurface,DD_EnumDisplayModes,DD_EnumSurfaces,DD_FlipToGDISurface,DD_GetCaps,DD_GetDisplayMode,DD_GetFourCCCodes,DD_GetGDISurface,DD_GetMonitorFrequency,DD_GetScanLine,DD_GetVerticalBlankStatus,DD_Initialize,DD_RestoreDisplayMode,DD_SetCooperativeLevel,DD_SetDisplayMode,DD_WaitForVerticalBlank,DD_GetAvailableVidMem,DD_GetSurfaceFromDC,DD_RestoreAllSurfaces,DD_TestCooperativeLevel,DD_GetDeviceIdentifier,DD_StartModeTest,DD_EvaluateMode,DD_N };
static const MDef dd_m[DD_N]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"Compact",1},{"CreateClipper",4},{"CreatePalette",5},{"CreateSurface",4},{"DuplicateSurface",3},{"EnumDisplayModes",5},{"EnumSurfaces",5},{"FlipToGDISurface",1},{"GetCaps",3},{"GetDisplayMode",2},{"GetFourCCCodes",3},{"GetGDISurface",2},{"GetMonitorFrequency",2},{"GetScanLine",2},{"GetVerticalBlankStatus",2},{"Initialize",2},{"RestoreDisplayMode",1},{"SetCooperativeLevel",3},{"SetDisplayMode",6},{"WaitForVerticalBlank",3},{"GetAvailableVidMem",4},{"GetSurfaceFromDC",3},{"RestoreAllSurfaces",1},{"TestCooperativeLevel",1},{"GetDeviceIdentifier",3},{"StartModeTest",4},{"EvaluateMode",3}};
enum { SF_QI,SF_AddRef,SF_Release,SF_AddAttachedSurface,SF_AddOverlayDirtyRect,SF_Blt,SF_BltBatch,SF_BltFast,SF_DeleteAttachedSurface,SF_EnumAttachedSurfaces,SF_EnumOverlayZOrders,SF_Flip,SF_GetAttachedSurface,SF_GetBltStatus,SF_GetCaps,SF_GetClipper,SF_GetColorKey,SF_GetDC,SF_GetFlipStatus,SF_GetOverlayPosition,SF_GetPalette,SF_GetPixelFormat,SF_GetSurfaceDesc,SF_Initialize,SF_IsLost,SF_Lock,SF_ReleaseDC,SF_Restore,SF_SetClipper,SF_SetColorKey,SF_SetOverlayPosition,SF_SetPalette,SF_Unlock,SF_UpdateOverlay,SF_UpdateOverlayDisplay,SF_UpdateOverlayZOrder,SF_GetDDInterface,SF_PageLock,SF_PageUnlock,SF_SetSurfaceDesc,SF_SetPrivateData,SF_GetPrivateData,SF_FreePrivateData,SF_GetUniquenessValue,SF_ChangeUniquenessValue,SF_SetPriority,SF_GetPriority,SF_SetLOD,SF_GetLOD,SF_N };
static const MDef sf_m[SF_N]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"AddAttachedSurface",2},{"AddOverlayDirtyRect",2},{"Blt",6},{"BltBatch",4},{"BltFast",6},{"DeleteAttachedSurface",3},{"EnumAttachedSurfaces",3},{"EnumOverlayZOrders",4},{"Flip",3},{"GetAttachedSurface",3},{"GetBltStatus",2},{"GetCaps",2},{"GetClipper",2},{"GetColorKey",3},{"GetDC",2},{"GetFlipStatus",2},{"GetOverlayPosition",3},{"GetPalette",2},{"GetPixelFormat",2},{"GetSurfaceDesc",2},{"Initialize",3},{"IsLost",1},{"Lock",5},{"ReleaseDC",2},{"Restore",1},{"SetClipper",2},{"SetColorKey",3},{"SetOverlayPosition",3},{"SetPalette",2},{"Unlock",2},{"UpdateOverlay",6},{"UpdateOverlayDisplay",2},{"UpdateOverlayZOrder",3},{"GetDDInterface",2},{"PageLock",2},{"PageUnlock",2},{"SetSurfaceDesc",3},{"SetPrivateData",5},{"GetPrivateData",4},{"FreePrivateData",2},{"GetUniquenessValue",2},{"ChangeUniquenessValue",1},{"SetPriority",2},{"GetPriority",2},{"SetLOD",2},{"GetLOD",2}};
static hle_fn dd_impl[DD_N], sf_impl[SF_N];

static uint32_t vt_clip=0, vt_pal=0;
static void dd_CreateClipper(void){ uint32_t o=ga_alloc(8,1); wr32(o,vt_clip); wr32(A(2),o); RET(0); }
static void dd_CreatePalette(void){ uint32_t o=ga_alloc(8,1); wr32(o,vt_pal); wr32(A(3),o); RET(0); }
static const MDef cl_m[9]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetClipList",4},{"GetHWnd",2},{"Initialize",3},{"IsClipListChanged",2},{"SetClipList",3},{"SetHWnd",3}};
static const MDef pl_m[7]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetCaps",2},{"GetEntries",5},{"Initialize",4},{"SetEntries",5}};
static hle_fn cl_impl[9], pl_impl[7];
static void ddraw_DirectDrawCreateEx(void){
    /* (guid, ppDD, iid, outer) */
    if(!dd_obj){ dd_obj=ga_alloc(8,1); wr32(dd_obj,vt_dd); }
    wr32(A(1),dd_obj); RET(0);
}
void ddraw_register(void){
    const char *e=getenv("RBO_DUMP_EVERY"); g_dump_every=e?atoi(e):0;
    for(int i=0;i<DD_N;i++) dd_impl[i]=com_default;
    dd_impl[DD_QI]=com_qi; dd_impl[DD_AddRef]=com_addref; dd_impl[DD_Release]=com_release;
    dd_impl[DD_CreateSurface]=dd_CreateSurface; dd_impl[DD_SetDisplayMode]=dd_SetDisplayMode; dd_impl[DD_GetDisplayMode]=dd_GetDisplayMode;
    dd_impl[DD_SetCooperativeLevel]=dd_SetCooperativeLevel; dd_impl[DD_EnumDisplayModes]=dd_EnumDisplayModes; dd_impl[DD_GetCaps]=dd_GetCaps;
    dd_impl[DD_CreateClipper]=dd_CreateClipper; dd_impl[DD_CreatePalette]=dd_CreatePalette;
    dd_impl[DD_GetAvailableVidMem]=dd_GetAvailableVidMem; dd_impl[DD_WaitForVerticalBlank]=dd_WaitForVerticalBlank;
    for(int i=0;i<SF_N;i++) sf_impl[i]=com_default;
    sf_impl[SF_QI]=com_qi; sf_impl[SF_AddRef]=com_addref; sf_impl[SF_Release]=com_release;
    sf_impl[SF_GetAttachedSurface]=sf_GetAttachedSurface; sf_impl[SF_Flip]=sf_Flip; sf_impl[SF_Blt]=sf_Blt; sf_impl[SF_BltFast]=sf_BltFast;
    sf_impl[SF_Lock]=sf_Lock; sf_impl[SF_Unlock]=sf_Unlock; sf_impl[SF_GetSurfaceDesc]=sf_GetSurfaceDesc; sf_impl[SF_GetPixelFormat]=sf_GetPixelFormat;
    sf_impl[SF_SetColorKey]=sf_SetColorKey; sf_impl[SF_IsLost]=sf_IsLost;
    static Iface dd={"IDirectDraw7",DD_N,dd_m,dd_impl}, sf={"IDirectDrawSurface7",SF_N,sf_m,sf_impl};
    for(int i=0;i<9;i++) cl_impl[i]=com_default; cl_impl[0]=com_qi; cl_impl[1]=com_addref; cl_impl[2]=com_release;
    for(int i=0;i<7;i++) pl_impl[i]=com_default; pl_impl[0]=com_qi; pl_impl[1]=com_addref; pl_impl[2]=com_release;
    static Iface cl={"IDirectDrawClipper",9,cl_m,cl_impl}, pl={"IDirectDrawPalette",7,pl_m,pl_impl};
    vt_clip=make_vtable(&cl); vt_pal=make_vtable(&pl);
    vt_dd=make_vtable(&dd); vt_surf=make_vtable(&sf); d3d_register();
    hle_register("DDRAW.dll","DirectDrawCreateEx",4,ddraw_DirectDrawCreateEx);
}

/* ========================= DirectInput 8 ========================= */
static uint32_t vt_di=0, vt_dev=0, di_obj=0;
static uint8_t keystate[256];
static int dev_kind[64]; static int ndev=0;   /* 0=keyboard 1=mouse 2=joystick */
static void di_CreateDevice(void){
    /* (this, guid*, out, outer) : identify by first dword of GUID */
    uint32_t g=A(1); uint32_t d1=g?rd32(g):0;
    int kind = d1==0x6F1D2B61?0 : d1==0x6F1D2B60?1 : 2;
    uint32_t o=ga_alloc(8,1); wr32(o,vt_dev); wr32(o+4,ndev); dev_kind[ndev++]=kind;
    fprintf(stderr,"[DInput] CreateDevice guid=%08x kind=%d\n",d1,kind);
    wr32(A(2),o); RET(0);
}
static void di_EnumDevices(void){ fprintf(stderr,"[DInput] EnumDevices type=%u (no devices)\n",A(1)); RET(0); }
static void dev_GetDeviceState(void){
    uint32_t sz=A(1), buf=A(2); int k=dev_kind[rd32(A(0)+4)];
    memset(gmem+buf,0,sz);
    if(k==0&&sz>=256) memcpy(gmem+buf,keystate,256);
    RET(0);
}
static void dev_GetCapabilities(void){ RET(0x80004005u); }
static void dev_Acquire(void){ RET(0); }
enum { IN_QI,IN_AddRef,IN_Release,IN_CreateDevice,IN_EnumDevices,IN_GetDeviceStatus,IN_RunControlPanel,IN_Initialize,IN_FindDevice,IN_EnumDevicesBySemantics,IN_ConfigureDevices,IN_N };
static const MDef in_m[IN_N]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"CreateDevice",4},{"EnumDevices",5},{"GetDeviceStatus",2},{"RunControlPanel",3},{"Initialize",3},{"FindDevice",4},{"EnumDevicesBySemantics",6},{"ConfigureDevices",5}};
enum { DV_QI,DV_AddRef,DV_Release,DV_GetCapabilities,DV_EnumObjects,DV_GetProperty,DV_SetProperty,DV_Acquire,DV_Unacquire,DV_GetDeviceState,DV_GetDeviceData,DV_SetDataFormat,DV_SetEventNotification,DV_SetCooperativeLevel,DV_GetObjectInfo,DV_GetDeviceInfo,DV_RunControlPanel,DV_Initialize,DV_CreateEffect,DV_EnumEffects,DV_GetEffectInfo,DV_GetForceFeedbackState,DV_SendForceFeedbackCommand,DV_EnumCreatedEffectObjects,DV_Escape,DV_Poll,DV_SendDeviceData,DV_EnumEffectsInFile,DV_WriteEffectToFile,DV_BuildActionMap,DV_SetActionMap,DV_GetImageInfo,DV_N };
static const MDef dv_m[DV_N]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetCapabilities",2},{"EnumObjects",4},{"GetProperty",3},{"SetProperty",3},{"Acquire",1},{"Unacquire",1},{"GetDeviceState",3},{"GetDeviceData",5},{"SetDataFormat",2},{"SetEventNotification",2},{"SetCooperativeLevel",3},{"GetObjectInfo",4},{"GetDeviceInfo",2},{"RunControlPanel",3},{"Initialize",4},{"CreateEffect",4},{"EnumEffects",4},{"GetEffectInfo",3},{"GetForceFeedbackState",2},{"SendForceFeedbackCommand",2},{"EnumCreatedEffectObjects",3},{"Escape",2},{"Poll",1},{"SendDeviceData",4},{"EnumEffectsInFile",5},{"WriteEffectToFile",5},{"BuildActionMap",4},{"SetActionMap",4},{"GetImageInfo",2}};
static hle_fn in_impl[IN_N], dv_impl[DV_N];
static void di_DirectInput8Create(void){ /* (hinst,ver,riid,ppv,outer) */
    if(!di_obj){ di_obj=ga_alloc(8,1); wr32(di_obj,vt_di); }
    wr32(A(3),di_obj); RET(0);
}
void dinput_register(void){
    for(int i=0;i<IN_N;i++) in_impl[i]=com_default; for(int i=0;i<DV_N;i++) dv_impl[i]=com_default;
    in_impl[IN_QI]=com_qi; in_impl[IN_AddRef]=com_addref; in_impl[IN_Release]=com_release; in_impl[IN_CreateDevice]=di_CreateDevice; in_impl[IN_EnumDevices]=di_EnumDevices;
    dv_impl[DV_QI]=com_qi; dv_impl[DV_AddRef]=com_addref; dv_impl[DV_Release]=com_release; dv_impl[DV_GetDeviceState]=dev_GetDeviceState; dv_impl[DV_GetCapabilities]=dev_GetCapabilities; dv_impl[DV_Acquire]=dev_Acquire;
    static Iface in={"IDirectInput8A",IN_N,in_m,in_impl}, dv={"IDirectInputDevice8A",DV_N,dv_m,dv_impl};
    vt_di=make_vtable(&in); vt_dev=make_vtable(&dv);
    hle_register("DINPUT8.dll","DirectInput8Create",5,di_DirectInput8Create);
}
void dx_set_key(int dik,int down){ if(dik>=0&&dik<256) keystate[dik]=down?0x80:0; }

/* ========================= DirectSound ========================= */
#include <math.h>
typedef struct {
    int used, primary, playing, looping;
    uint32_t buf, size;
    int chans, bits, rate, freq;         /* source format, current playback frequency */
    double pos;                           /* play position in bytes as of the last tick */
    uint64_t t_tick;                      /* time of last tick (us) */
    int vol, pan;                         /* hundredths of dB */
    int nnotify; uint32_t noff[16], nev[16];
} SBuf;
static SBuf SB[512];
static uint32_t vt_ds=0, vt_dsb=0, ds_obj=0;
#define OUT_RATE 44100
#define RING_FRAMES 65536
static int16_t ring[RING_FRAMES*2]; static uint32_t ring_head=0, ring_tail=0; int g_audio_ring_on=0;
int rbo_audio_pull(int16_t *out,int frames){
    uint32_t tail=__atomic_load_n(&ring_tail,__ATOMIC_RELAXED), head=__atomic_load_n(&ring_head,__ATOMIC_ACQUIRE); int got=0;
    while(got<frames&&tail!=head){ out[2*got]=ring[(tail%RING_FRAMES)*2]; out[2*got+1]=ring[(tail%RING_FRAMES)*2+1]; tail++; got++; }
    __atomic_store_n(&ring_tail,tail,__ATOMIC_RELEASE);
    for(int i=got;i<frames;i++){ out[2*i]=0; out[2*i+1]=0; }
    return got;
}
static FILE *g_wav=NULL; static uint32_t g_wav_frames=0; static int wav_init=0;
static uint64_t last_tick_us=0; static double tick_carry=0;
static void wav_close(void){
    if(!g_wav) return; uint32_t data=g_wav_frames*4, riff=36+data;
    fseek(g_wav,4,SEEK_SET); fwrite(&riff,4,1,g_wav); fseek(g_wav,40,SEEK_SET); fwrite(&data,4,1,g_wav); fclose(g_wav); g_wav=NULL;
}
static void wav_open(void){
    wav_init=1; const char *p=getenv("RBO_WAV"); if(!p) return; g_wav=fopen(p,"wb"); if(!g_wav) return;
    uint8_t hd[44]={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,0x44,0xAC,0,0,0x10,0xB1,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    fwrite(hd,1,44,g_wav); atexit(wav_close);
}
static SBuf *sb_of(uint32_t o){ return &SB[rd32(o+4)]; }
static double gain_db(int hund){ if(hund<=-10000) return 0.0; if(hund>0) hund=0; return pow(10.0,hund/2000.0); }
static int bytes_per_frame(SBuf*b){ return b->chans*(b->bits/8); }
static inline double sample_at(SBuf*b,uint32_t frame,int ch){
    uint32_t off=frame*bytes_per_frame(b)+ch*(b->bits/8);
    if(off+ (b->bits/8) > b->size) return 0;
    uint8_t *p=gmem+b->buf+off;
    if(b->bits==8) return ((int)p[0]-128)/128.0;
    return (int16_t)(p[0]|(p[1]<<8))/32768.0;
}
/* advance one buffer by nout output frames, mixing into acc (stereo float) when acc!=NULL */
static void sb_advance(SBuf*b,int nout,float *acc){
    if(!b->playing||b->primary||!b->size||!b->buf) return;
    int bpf=bytes_per_frame(b); if(bpf<=0) return;
    double step=(double)b->freq/OUT_RATE*bpf;      /* bytes per output frame */
    double gl=gain_db(b->vol), gr=gl; if(b->pan>0) gl*=gain_db(-b->pan); else if(b->pan<0) gr*=gain_db(b->pan);
    for(int i=0;i<nout;i++){
        if(b->pos>=b->size){ if(b->looping){ b->pos=fmod(b->pos,(double)b->size); } else { b->playing=0; b->pos=0; return; } }
        uint32_t f=(uint32_t)(b->pos/bpf);
        if(acc){
            double l=sample_at(b,f,0), r=b->chans>1?sample_at(b,f,1):l;
            acc[2*i]+=(float)(l*gl); acc[2*i+1]+=(float)(r*gr);
        }
        b->pos+=step;
    }
}
void ds_tick(void){
    if(!wav_init) wav_open();
    uint64_t now=hle_now_us(); if(!last_tick_us){ last_tick_us=now; return; }
    if(now<=last_tick_us) return;
    uint64_t dt=now-last_tick_us; if(dt>500000) dt=500000;
    double nf=dt*(double)OUT_RATE/1e6+tick_carry; int n=(int)nf; tick_carry=nf-n; last_tick_us=now;
    if(n<=0) return;
    float *acc=calloc(2*(size_t)n,sizeof(float));
    for(int i=0;i<512;i++) if(SB[i].used){
        SBuf *b=&SB[i]; b->t_tick=now; double p0=b->pos; int was=b->playing;
        sb_advance(b,n,acc);
        if(b->nnotify && was){
            double p1=b->pos; int wrapped=(p1<p0)||(!b->playing);
            for(int k=0;k<b->nnotify;k++){ uint32_t off=b->noff[k];
                if(off==0xFFFFFFFFu){ if(!b->playing) hle_set_event(b->nev[k]); continue; }
                int hit = !wrapped ? (off>p0 && off<=p1) : (off>p0 || off<=p1);
                if(hit) hle_set_event(b->nev[k]); }
        }
    }
    if(g_audio_ring_on){
        int16_t *o2=malloc(4*(size_t)n);
        for(int i=0;i<2*n;i++){ float v=acc[i]; if(v>1)v=1; if(v<-1)v=-1; o2[i]=(int16_t)(v*32767); }
        uint32_t head=__atomic_load_n(&ring_head,__ATOMIC_RELAXED), tail=__atomic_load_n(&ring_tail,__ATOMIC_ACQUIRE);
        for(int i=0;i<n;i++){ if(head-tail>=RING_FRAMES) break; ring[(head%RING_FRAMES)*2]=o2[2*i]; ring[(head%RING_FRAMES)*2+1]=o2[2*i+1]; head++; }
        __atomic_store_n(&ring_head,head,__ATOMIC_RELEASE); free(o2);
    }
    if(g_wav){
        int16_t *o=malloc(4*(size_t)n);
        for(int i=0;i<2*n;i++){ float v=acc[i]; if(v>1)v=1; if(v<-1)v=-1; o[i]=(int16_t)(v*32767); }
        fwrite(o,4,n,g_wav); g_wav_frames+=n; free(o);
    }
    free(acc);
}
static double cur_pos(SBuf*b){   /* position extrapolated to "now" without consuming audio */
    if(!b->playing||!b->size) return b->pos;
    uint64_t now=hle_now_us(); double el=(now>last_tick_us)?(double)(now-last_tick_us):0;
    double p=b->pos+el*b->freq*bytes_per_frame(b)/1e6;
    if(b->looping&&p>=b->size) p=fmod(p,(double)b->size); else if(p>=b->size) p=b->size;
    return p;
}
static uint32_t vt_notify=0;
static void dn_SetNotificationPositions(void){
    SBuf *b=sb_of(A(0)); uint32_t n=A(1), arr=A(2); if(n>16) n=16; b->nnotify=(int)n;
    for(uint32_t i=0;i<n;i++){ b->noff[i]=rd32(arr+8*i); b->nev[i]=rd32(arr+8*i+4); if(g_trace) fprintf(stderr,"[DSound] notify buf %d off=%u ev=%08x\n",(int)(b-SB),b->noff[i],b->nev[i]); }
    RET(0);
}
static void ds_CreateSoundBuffer(void){
    uint32_t d=A(1); uint32_t flags=rd32(d+4), bytes=rd32(d+8), wf=rd32(d+16);
    int id=-1; for(int i=0;i<512;i++) if(!SB[i].used){id=i;break;}
    SBuf *b=&SB[id]; memset(b,0,sizeof *b); b->used=1; b->primary=(flags&1)!=0; b->size=bytes;
    if(!b->primary&&bytes) b->buf=ga_alloc(bytes,1);
    b->chans=wf?rd16(wf+2):2; b->rate=wf?rd32(wf+4):44100; b->bits=wf?rd16(wf+14):16; if(b->chans<1)b->chans=1; if(b->bits!=8&&b->bits!=16) b->bits=16;
    b->freq=b->rate; b->vol=0; b->pan=0;
    if(g_trace) fprintf(stderr,"[DSound] CreateSoundBuffer %s bytes=%u %dch %dbit %dHz flags=%08x\n",b->primary?"primary":"secondary",bytes,b->chans,b->bits,b->rate,flags);
    uint32_t o=ga_alloc(8,1); wr32(o,vt_dsb); wr32(o+4,id); wr32(A(2),o); RET(0);
}
static void dsb_Lock(void){ /* (this, off, bytes, p1*, b1*, p2*, b2*, flags) */
    SBuf *b=sb_of(A(0)); uint32_t off=A(1), n=A(2);
    if(A(7)&2) n=b->size; if(A(7)&1) off=(uint32_t)cur_pos(b);
    if(b->nnotify&&getenv("RBO_DSTRACE")){ static int lc=0; if(lc++<40) fprintf(stderr,"[DSound] frame %d Lock id=%d off=%u bytes=%u flags=%u size=%u pos=%.0f\n",g_frames,(int)(b-SB),off,n,A(7),b->size,cur_pos(b)); }
    if(off>b->size) off=0; if(off+n>b->size) n=b->size-off;
    wr32(A(3),b->buf+off); wr32(A(4),n); if(A(5))wr32(A(5),0); if(A(6))wr32(A(6),0); RET(0);
}
static void dsb_Play(void){ SBuf *b=sb_of(A(0)); if(g_trace) fprintf(stderr,"[DSound] frame %d Play id=%d size=%u loop=%d\n",g_frames,(int)(b-SB),b->size,(int)(A(3)&1)); b->playing=1; b->looping=(A(3)&1)!=0; RET(0); }
static void dsb_Stop(void){
    SBuf *b=sb_of(A(0)); b->pos=cur_pos(b); int was=b->playing; b->playing=0;
    if(g_trace) fprintf(stderr,"[DSound] frame %d Stop id=%d (was playing=%d, %d notify)\n",g_frames,(int)(b-SB),was,b->nnotify);
    for(int k=0;k<b->nnotify;k++) if(b->noff[k]==0xFFFFFFFFu) hle_set_event(b->nev[k]);
    RET(0);
}
static void dsb_GetCurrentPosition(void){
    SBuf *b=sb_of(A(0)); uint32_t pos=(uint32_t)cur_pos(b);
    if(b->size&&pos>=b->size) pos=b->size-1;
    if(A(1)) wr32(A(1),pos); if(A(2)) wr32(A(2),pos); RET(0);
}
static void dsb_SetCurrentPosition(void){ SBuf *b=sb_of(A(0)); b->pos=A(1); RET(0); }
static void dsb_GetStatus(void){ SBuf *b=sb_of(A(0)); uint32_t st=0; if(b->playing) st|=1|(b->looping?4:0); wr32(A(1),st); RET(0); }
static void dsb_Unlock(void){
    SBuf *b=sb_of(A(0)); uint32_t p1=A(1), n1=A(2);
    { static FILE *raw=NULL; static int rawid=-1; const char *rp=getenv("RBO_DUMPSTREAM");
      if(rp&&b->nnotify&&(rawid<0||rawid==(int)(b-SB))){ if(!raw){ raw=fopen(rp,"wb"); rawid=(int)(b-SB); } if(raw&&n1&&p1+n1<MEM_SIZE){ fwrite(gmem+p1,1,n1,raw); fflush(raw); } } }
    if(g_trace>1||getenv("RBO_DSTRACE")){ uint64_t sum=0; for(uint32_t i=0;i<n1&&p1+i<MEM_SIZE;i++) sum+=gmem[p1+i];
        static int cnt=0; if(b->nnotify && cnt++<60) fprintf(stderr,"[DSound] frame %d Unlock id=%d off=%u bytes=%u sum=%llu pos=%.0f play=%d\n",g_frames,(int)(b-SB),p1-b->buf,n1,(unsigned long long)sum,b->pos,b->playing); }
    RET(0);
}
static void dsb_SetVolume(void){ SBuf *b=sb_of(A(0)); b->vol=(int)A(1); RET(0); }
static void dsb_GetVolume(void){ wr32(A(1),(uint32_t)sb_of(A(0))->vol); RET(0); }
static void dsb_SetPan(void){ SBuf *b=sb_of(A(0)); b->pan=(int)A(1); RET(0); }
static void dsb_GetPan(void){ wr32(A(1),(uint32_t)sb_of(A(0))->pan); RET(0); }
static void dsb_SetFrequency(void){ SBuf *b=sb_of(A(0)); uint32_t f=A(1); b->freq=(f==0||f==(uint32_t)-1)?b->rate:(int)f; RET(0); }
static void dsb_GetFrequency(void){ wr32(A(1),(uint32_t)sb_of(A(0))->freq); RET(0); }
static void dsb_GetCaps(void){ uint32_t p=A(1); SBuf *b=sb_of(A(0)); memset(gmem+p+4,0,rd32(p)>=20?rd32(p)-4:16); wr32(p+8,b->size); RET(0); }
static void ds_Duplicate(void){ SBuf *b=sb_of(A(1)); int id=-1; for(int i=0;i<512;i++) if(!SB[i].used){id=i;break;} SB[id]=*b; SB[id].playing=0; SB[id].pos=0;
    uint32_t o=ga_alloc(8,1); wr32(o,vt_dsb); wr32(o+4,id); wr32(A(2),o); RET(0); }
enum { DS_QI,DS_AddRef,DS_Release,DS_CreateSoundBuffer,DS_GetCaps,DS_DuplicateSoundBuffer,DS_SetCooperativeLevel,DS_Compact,DS_GetSpeakerConfig,DS_SetSpeakerConfig,DS_Initialize,DS_N };
static const MDef ds_m[DS_N]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"CreateSoundBuffer",4},{"GetCaps",2},{"DuplicateSoundBuffer",3},{"SetCooperativeLevel",3},{"Compact",1},{"GetSpeakerConfig",2},{"SetSpeakerConfig",2},{"Initialize",2}};
enum { SB_QI,SB_AddRef,SB_Release,SB_GetCaps,SB_GetCurrentPosition,SB_GetFormat,SB_GetVolume,SB_GetPan,SB_GetFrequency,SB_GetStatus,SB_Initialize,SB_Lock,SB_Play,SB_SetCurrentPosition,SB_SetFormat,SB_SetVolume,SB_SetPan,SB_SetFrequency,SB_Stop,SB_Unlock,SB_Restore,SB_N };
static const MDef sb_m[SB_N]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetCaps",2},{"GetCurrentPosition",3},{"GetFormat",4},{"GetVolume",2},{"GetPan",2},{"GetFrequency",2},{"GetStatus",2},{"Initialize",3},{"Lock",8},{"Play",4},{"SetCurrentPosition",2},{"SetFormat",2},{"SetVolume",2},{"SetPan",2},{"SetFrequency",2},{"Stop",1},{"Unlock",5},{"Restore",1}};
static hle_fn ds_impl[DS_N], sb_impl[SB_N];
static void ds_Create(void){ if(!ds_obj){ ds_obj=ga_alloc(8,1); wr32(ds_obj,vt_ds);} wr32(A(1),ds_obj); RET(0); }
void dsound_register(void){
    for(int i=0;i<DS_N;i++) ds_impl[i]=com_default; for(int i=0;i<SB_N;i++) sb_impl[i]=com_default;
    ds_impl[DS_QI]=com_qi; ds_impl[DS_AddRef]=com_addref; ds_impl[DS_Release]=com_release; ds_impl[DS_CreateSoundBuffer]=ds_CreateSoundBuffer; ds_impl[DS_DuplicateSoundBuffer]=ds_Duplicate;
    sb_impl[SB_QI]=com_qi; sb_impl[SB_AddRef]=com_addref; sb_impl[SB_Release]=com_release; sb_impl[SB_Lock]=dsb_Lock; sb_impl[SB_Play]=dsb_Play; sb_impl[SB_Stop]=dsb_Stop;
    sb_impl[SB_Unlock]=dsb_Unlock; sb_impl[SB_SetVolume]=dsb_SetVolume; sb_impl[SB_GetVolume]=dsb_GetVolume; sb_impl[SB_SetPan]=dsb_SetPan; sb_impl[SB_GetPan]=dsb_GetPan; sb_impl[SB_SetFrequency]=dsb_SetFrequency; sb_impl[SB_GetFrequency]=dsb_GetFrequency; sb_impl[SB_GetCaps]=dsb_GetCaps;
    sb_impl[SB_GetCurrentPosition]=dsb_GetCurrentPosition; sb_impl[SB_SetCurrentPosition]=dsb_SetCurrentPosition; sb_impl[SB_GetStatus]=dsb_GetStatus;
    static Iface ds={"IDirectSound",DS_N,ds_m,ds_impl}, sb={"IDirectSoundBuffer",SB_N,sb_m,sb_impl};
    static const MDef dn_m[4]={{"QueryInterface",3},{"AddRef",1},{"Release",1},{"SetNotificationPositions",3}};
    static hle_fn dn_impl[4]; dn_impl[0]=com_qi; dn_impl[1]=com_addref; dn_impl[2]=com_release; dn_impl[3]=dn_SetNotificationPositions;
    static Iface dn={"IDirectSoundNotify",4,dn_m,dn_impl};
    vt_ds=make_vtable(&ds); vt_dsb=make_vtable(&sb); vt_notify=make_vtable(&dn);
    hle_register("DSOUND.dll","Ordinal11",3,ds_Create);
}
