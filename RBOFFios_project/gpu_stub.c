/* gpu_stub.c - software stand-in for gpu_raster.m, used only by test_gpu.c on a Linux host.
 * It implements the gpu_raster.h API with the same inputs/semantics as the Metal shader (float maths, pixel-centre coverage with the same
 * top-left rule), so the test can check d3d.c's GPU routing (state mapping, vertex building, residency/readback, texture upload) against the
 * CPU rasterizer.  The Metal shader in gpu_raster.m is a line-by-line port of frag() below. */
#include "gpu_raster.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { uint32_t *px; uint8_t *key; int w,h; } STex;
static STex tx[1024]; static uint32_t *rt; static int rtw,rth;
static unsigned long n_sync,n_vert,n_run;

int gpu_init(void){ return 1; }
const char *gpu_error(void){ return ""; }
int gpu_failed(void){ return 0; }
void gpu_stats(unsigned long *s,double *w,unsigned long *v,unsigned long *r){ if(s)*s=n_sync; if(w)*w=0; if(v)*v=n_vert; if(r)*r=n_run; }
void gpu_sync(void){ n_sync++; }
void gpu_rt_set(int w,int h){ if(rt&&rtw==w&&rth==h) return; free(rt); rt=calloc((size_t)w*h,4); rtw=w; rth=h; }
void gpu_rt_upload(const uint32_t *a,int w,int h){ if(w==rtw&&h==rth) memcpy(rt,a,(size_t)w*h*4); }
void gpu_rt_read(uint32_t *a){ memcpy(a,rt,(size_t)rtw*rth*4); }
void gpu_tex_upload(int id,const uint32_t *a,const uint8_t *k,int w,int h){
    STex *t=&tx[id]; free(t->px); free(t->key); t->px=malloc((size_t)w*h*4); memcpy(t->px,a,(size_t)w*h*4);
    t->key=k?malloc((size_t)w*h):NULL; if(k) for(size_t i=0;i<(size_t)w*h;i++) t->key[i]=k[i]?255:0; t->w=w; t->h=h;
}

static int addr_i(int i,int n,int mode){
    if(mode==2) return i<0?0:(i>=n?n-1:i);
    if(mode==1){ int p=((i%(2*n))+2*n)%(2*n); return p<n?p:2*n-1-p; }
    return ((i%n)+n)%n;
}
static void texel(const STex *t,int x,int y,float c[4],float *k,int mode){
    x=addr_i(x,t->w,mode); y=addr_i(y,t->h,mode); size_t i=(size_t)y*t->w+x; uint32_t p=t->px[i];
    c[0]=((p>>16)&255)/255.0f; c[1]=((p>>8)&255)/255.0f; c[2]=(p&255)/255.0f; c[3]=(p>>24)/255.0f; *k=t->key?t->key[i]/255.0f:0.0f;
}
static float fmin1(float a){ return a>1.0f?1.0f:a; }
static int bf(int f,float sc[4],float dc[4],int ch,float *out){   /* factor for channel ch (0..2) */
    float sa=sc[3], da=dc[3], s=sc[ch], d=dc[ch];
    switch(f){ case 1:*out=0;break; case 2:*out=1;break; case 3:*out=s;break; case 4:*out=1-s;break; case 5:*out=sa;break; case 6:*out=1-sa;break;
               case 7:*out=da;break; case 8:*out=1-da;break; case 9:*out=d;break; case 10:*out=1-d;break; default:*out=1; }
    return 0;
}
/* returns 0 when the fragment is discarded; col = rgba after texture stage */
static int frag(const GpuState *st,const float diff[4],float u,float v,float col[4]){
    float cc[3]={diff[0],diff[1],diff[2]}, aa=diff[3];
    if(st->tex_on){
        const STex *t=&tx[st->tex_id]; float tc[4], k; int mode=st->addr;
        float tcol[4]={0,0,0,0}, kk=0;
        if(st->point){
            float tw=(float)st->tw, th=(float)st->th;
            float uu=(floorf(u*tw+0.004f)+0.5f)/tw, vv=(floorf(v*th+0.004f)+0.5f)/th;
            texel(t,(int)floorf(uu*tw),(int)floorf(vv*th),tcol,&kk,mode);
        } else {
            float fx=u*st->tw-0.5f, fy=v*st->th-0.5f; int x0=(int)floorf(fx), y0=(int)floorf(fy); float ax=fx-x0, ay=fy-y0;
            float w4[4]={(1-ax)*(1-ay),ax*(1-ay),(1-ax)*ay,ax*ay};
            for(int j=0;j<2;j++) for(int i=0;i<2;i++){ texel(t,x0+i,y0+j,tc,&k,mode); float w=w4[j*2+i]; for(int c=0;c<4;c++) tcol[c]+=tc[c]*w; kk+=k*w; }
        }
        if(st->use_key && kk>0.999f) return 0;
        float tf[4]={st->tfac[0],st->tfac[1],st->tfac[2],st->tfac[3]};
        float c1[3],c2[3]; for(int i=0;i<3;i++){
            c1[i]=st->ca1==2?tcol[i]:(st->ca1==3?tf[i]:diff[i]); c2[i]=st->ca2==2?tcol[i]:(st->ca2==3?tf[i]:diff[i]); }
        for(int i=0;i<3;i++) switch(st->cop){ case 2:cc[i]=c1[i];break; case 3:cc[i]=c2[i];break; case 5:cc[i]=fmin1(c1[i]*c2[i]*2);break;
            case 6:cc[i]=fmin1(c1[i]*c2[i]*4);break; case 7:cc[i]=fmin1(c1[i]+c2[i]);break; default:cc[i]=c1[i]*c2[i]; }
        float a1=st->aa1==2?tcol[3]:(st->aa1==3?tf[3]:diff[3]), a2=st->aa2==2?tcol[3]:(st->aa2==3?tf[3]:diff[3]);
        switch(st->aop){ case 1:aa=diff[3];break; case 2:aa=a1;break; case 3:aa=a2;break; case 5:aa=fmin1(a1*a2*2);break; case 7:aa=fmin1(a1+a2);break; default:aa=a1*a2; }
    }
    if(st->atest){ int a8=(int)(aa*255.0f+0.5f), r=st->aref, ok=1;
        switch(st->afunc){ case 1:ok=0;break; case 2:ok=a8<r;break; case 3:ok=a8==r;break; case 4:ok=a8<=r;break; case 5:ok=a8>r;break; case 6:ok=a8!=r;break; case 7:ok=a8>=r;break; default:ok=1; }
        if(!ok) return 0; }
    col[0]=cc[0]; col[1]=cc[1]; col[2]=cc[2]; col[3]=aa; return 1;
}
static float edge(float ax,float ay,float bx,float by,float px,float py){ return (bx-ax)*(py-ay)-(by-ay)*(px-ax); }
static int is_tl(float ax,float ay,float bx,float by){ float dx=bx-ax, dy=by-ay; return (dy==0&&dx>0)||dy<0; }
static uint32_t q8(float v){ if(v<0)v=0; if(v>1)v=1; return (uint32_t)(v*255.0f+0.5f); }
void gpu_draw(const GpuState *st,const float *vs,int nv){
    n_vert+=nv; n_run++;
    for(int t=0;t+2<nv;t+=3){
        const float *A=vs+t*8,*B=vs+(t+1)*8,*C=vs+(t+2)*8;
        float area=edge(A[0],A[1],B[0],B[1],C[0],C[1]); if(area==0) continue;
        float minx=fminf(A[0],fminf(B[0],C[0])), maxx=fmaxf(A[0],fmaxf(B[0],C[0])), miny=fminf(A[1],fminf(B[1],C[1])), maxy=fmaxf(A[1],fmaxf(B[1],C[1]));
        int x0=(int)fmaxf(floorf(minx),(float)st->sx), x1=(int)fminf(ceilf(maxx),(float)(st->sx+st->sw)), y0=(int)fmaxf(floorf(miny),(float)st->sy), y1=(int)fminf(ceilf(maxy),(float)(st->sy+st->sh));
        int tl0=is_tl(B[0],B[1],C[0],C[1]), tl1=is_tl(C[0],C[1],A[0],A[1]), tl2=is_tl(A[0],A[1],B[0],B[1]);
        for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
            float px=x+0.5f, py=y+0.5f;
            float w0=edge(B[0],B[1],C[0],C[1],px,py), w1=edge(C[0],C[1],A[0],A[1],px,py), w2=edge(A[0],A[1],B[0],B[1],px,py);
            if(!(w0>0||(w0==0&&tl0))||!(w1>0||(w1==0&&tl1))||!(w2>0||(w2==0&&tl2))) continue;
            float l0=w0/area,l1=w1/area,l2=w2/area; float diff[4];
            for(int c=0;c<4;c++) diff[c]=l0*A[4+c]+l1*B[4+c]+l2*C[4+c];
            float u=l0*A[2]+l1*B[2]+l2*C[2], v=l0*A[3]+l1*B[3]+l2*C[3], col[4];
            if(!frag(st,diff,u,v,col)) continue;
            uint32_t *p=&rt[(size_t)y*rtw+x]; uint32_t d=*p; float dc[4]={((d>>16)&255)/255.0f,((d>>8)&255)/255.0f,(d&255)/255.0f,(d>>24)/255.0f};
            float out[4]={col[0],col[1],col[2],col[3]};
            if(st->blend){ for(int ch=0;ch<3;ch++){ float sfv,dfv; bf(st->sf,col,dc,ch,&sfv); bf(st->df,col,dc,ch,&dfv); out[ch]=col[ch]*sfv+dc[ch]*dfv; } }
            uint32_t r=q8(out[0]), g=q8(out[1]), b=q8(out[2]), a=st->write_alpha?q8(out[3]):(d>>24);
            *p=(a<<24)|(r<<16)|(g<<8)|b;
        }
    }
}
