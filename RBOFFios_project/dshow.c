/* Minimal DirectShow filter-graph emulation.
 *  - music: "RenderFile + Run/Stop/Position/Volume" (forwarded to the host's bgm hooks)
 *  - movies (.mpg/.mpeg): decoded in software with pl_mpeg (MPEG-1 video + MP2 audio; iOS has no decoder for them),
 *    video frames go straight to the host screen, audio is mixed into the DirectSound output (dx.c),
 *    and the graph reports EC_COMPLETE through the window message the game registered with IMediaEventEx::SetNotifyWindow. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "dxshared.h"
#include "pl_mpeg.h"
#include <string.h>
#include <strings.h>
#include <math.h>

typedef struct { int playing; char path[512]; int volume; double pos; } Bgm;
Bgm g_bgm;
static uint32_t vt_graph, vt_mc, vt_mev, vt_mpos, vt_aud, vt_vw, vt_seek, vt_bv;
static uint32_t graph_obj=0, mc_obj=0, mev_obj=0, mpos_obj=0, aud_obj=0, vw_obj=0, seek_obj=0, bv_obj=0;
static uint32_t ev_handle=0;
extern int g_cur_idx;
void bgm_host_play(const char *path,int volume);   /* platform hooks (weak defaults in main.c) */
void bgm_host_stop(void);
/* provided by dx.c / win.c */
void dx_host_submit(const uint32_t *bgra,int w,int h);
void dx_movie_audio_push(const float *interleaved,int frames);
void dx_movie_audio_clear(void);
void ds_tick(void);
void win_post_message(uint32_t hwnd,uint32_t msg,uint32_t wparam,uint32_t lparam);

#define E_ABORT_   0x80004004u
#define VFW_E_NOT_FOUND 0x80040216u
#define EC_COMPLETE 1

static void wide_to_ascii(uint32_t w,char *out,int cap){ int i=0; for(;i<cap-1;i++){ uint16_t c=rd16(w+2*i); if(!c)break; out[i]=(c<128)?(char)c:'?'; } out[i]=0; }
static void trace_call(void){ if(g_trace) fprintf(stderr,"[DShow] %s\n",hle_name(g_cur_idx)); }

/* ---------------------------------------------------------------- movie player ---------------------------------------------------------------- */
typedef struct {
    plm_t *plm;
    int open;            /* file opened and headers parsed */
    int running;         /* IMediaControl::Run was called and Stop was not (paused counts as running) */
    int paused, ended;
    int w,h; double duration; int rate;
    uint64_t last_us;    /* guest clock at the last pump */
    uint32_t *bgra;      /* converted current frame */
    double gain;         /* IBasicAudio volume as linear gain */
    float *rs; int rs_cap;   /* resample scratch (only when the file is not 44.1 kHz) */
    double rs_pos; float rs_last[2];
    /* IMediaEvent state */
    uint32_t notify_hwnd, notify_msg, notify_lparam; int ev_pending;
} Movie;
static Movie mv = { .gain = 1.0 };

static int is_movie_path(const char *p){
    const char *dot=strrchr(p,'.'); if(!dot) return 0;
    return !strcasecmp(dot,".mpg")||!strcasecmp(dot,".mpeg")||!strcasecmp(dot,".mpe")||!strcasecmp(dot,".m1v")||!strcasecmp(dot,".mpv");
}
static void movie_close(void){
    if(mv.plm){ plm_destroy(mv.plm); mv.plm=NULL; }
    free(mv.bgra); mv.bgra=NULL; free(mv.rs); mv.rs=NULL; mv.rs_cap=0;
    mv.open=mv.running=mv.paused=mv.ended=0; mv.ev_pending=0; mv.last_us=0; dx_movie_audio_clear();
}
static void on_video(plm_t *p,plm_frame_t *f,void *u){
    (void)p;(void)u;
    if(!mv.bgra||f->width!=(unsigned)mv.w||f->height!=(unsigned)mv.h) return;
    plm_frame_to_bgra(f,(uint8_t*)mv.bgra,mv.w*4);
    dx_host_submit(mv.bgra,mv.w,mv.h);
}
static void on_audio(plm_t *p,plm_samples_t *s,void *u){
    (void)p;(void)u;
    const float *in=s->interleaved; int n=(int)s->count;
    float tmp[PLM_AUDIO_SAMPLES_PER_FRAME*2];
    if(mv.rate==44100){
        for(int i=0;i<2*n;i++) tmp[i]=in[i]*(float)mv.gain;
        dx_movie_audio_push(tmp,n);
    } else if(mv.rate>0){    /* linear resample to the 44.1 kHz output rate */
        double step=(double)mv.rate/44100.0; int cap=(int)(n/step)+4;
        if(cap>mv.rs_cap){ free(mv.rs); mv.rs=malloc((size_t)cap*8); mv.rs_cap=cap; }
        int out=0; double pos=mv.rs_pos;
        while(pos<n && out<cap){
            int i0=(int)floor(pos); double fr=pos-i0; const float *a=(i0>=0)?in+2*i0:mv.rs_last; const float *b=(i0+1<n)?in+2*(i0+1):in+2*(n-1);
            mv.rs[2*out]=(float)((a[0]+(b[0]-a[0])*fr)*mv.gain); mv.rs[2*out+1]=(float)((a[1]+(b[1]-a[1])*fr)*mv.gain); out++; pos+=step;
        }
        mv.rs_pos=pos-n; mv.rs_last[0]=in[2*(n-1)]; mv.rs_last[1]=in[2*(n-1)+1];
        dx_movie_audio_push(mv.rs,out);
    }
}
static int movie_open(const char *gpath){
    movie_close();
    char host[1200]; if(!resolve_path(gpath,host,sizeof host)){ fprintf(stderr,"[Movie] file not found: %s\n",gpath); return 0; }
    mv.plm=plm_create_with_filename(host);
    if(!mv.plm){ fprintf(stderr,"[Movie] cannot open %s\n",host); return 0; }
    if(!plm_probe(mv.plm,5000*1024) && !plm_has_headers(mv.plm)){ fprintf(stderr,"[Movie] not an MPEG-1 program stream: %s\n",host); plm_destroy(mv.plm); mv.plm=NULL; return 0; }
    mv.w=plm_get_width(mv.plm); mv.h=plm_get_height(mv.plm); mv.duration=plm_get_duration(mv.plm); mv.rate=plm_get_samplerate(mv.plm);
    if(mv.w<=0||mv.h<=0){ plm_destroy(mv.plm); mv.plm=NULL; return 0; }
    mv.bgra=malloc((size_t)mv.w*mv.h*4); memset(mv.bgra,0,(size_t)mv.w*mv.h*4);
    plm_set_loop(mv.plm,0); plm_set_audio_enabled(mv.plm,plm_get_num_audio_streams(mv.plm)>0); plm_set_audio_stream(mv.plm,0);
    plm_set_audio_lead_time(mv.plm,0.1);
    plm_set_video_decode_callback(mv.plm,on_video,NULL); plm_set_audio_decode_callback(mv.plm,on_audio,NULL);
    mv.open=1; mv.rs_pos=0; mv.rs_last[0]=mv.rs_last[1]=0;
    fprintf(stderr,"[Movie] opened %s: %dx%d %.2f fps %.1fs audio=%d Hz\n",host,mv.w,mv.h,plm_get_framerate(mv.plm),mv.duration,mv.rate);
    return 1;
}
/* true while a movie owns the screen: dx.c then skips presenting the game's own surface */
int dshow_movie_active(void){ return mv.open && mv.running && !mv.ended; }
/* Advance the decoder by the guest-clock time since the last call. Called once per presented frame and from PeekMessage. */
void dshow_movie_pump(void){
    if(!mv.open||!mv.running||mv.paused||mv.ended) return;
    uint64_t now=hle_now_us();
    if(!mv.last_us){ mv.last_us=now; return; }
    double dt=(double)(now-mv.last_us)/1e6; mv.last_us=now;
    if(dt<=0) return; if(dt>0.1) dt=0.1;     /* after a stall do not fast-forward through the movie */
    plm_decode(mv.plm,dt);
    ds_tick();
    if(plm_has_ended(mv.plm)){
        mv.ended=1; mv.ev_pending=1;
        if(g_trace||1) fprintf(stderr,"[Movie] ended at %.2fs\n",plm_get_time(mv.plm));
        if(mv.notify_hwnd) win_post_message(mv.notify_hwnd,mv.notify_msg,0,mv.notify_lparam);
    }
}
static double movie_time(void){ return mv.plm?plm_get_time(mv.plm):0.0; }

/* ---------------------------------------------------------------- COM stubs ---------------------------------------------------------------- */
static void d_zero(void){ trace_call(); RET(0); }
static void d_addref(void){ RET(1); }
static void d_release(void){ RET(0); }
static void d_qi(void){
    uint32_t d1=A(1)?rd32(A(1)):0; uint32_t obj=0;
    switch(d1){
    case 0x56a868b1: obj=mc_obj; break;      /* IMediaControl */
    case 0x56a868c0: obj=mev_obj; break;     /* IMediaEventEx */
    case 0x56a868b6: obj=mev_obj; break;     /* IMediaEvent */
    case 0x56a868b2: obj=mpos_obj; break;    /* IMediaPosition */
    case 0x56a868b3: obj=aud_obj; break;     /* IBasicAudio */
    case 0x56a868b4: obj=vw_obj; break;      /* IVideoWindow */
    case 0x56a868b5: obj=bv_obj; break;      /* IBasicVideo */
    case 0x36b73880: obj=seek_obj; break;    /* IMediaSeeking */
    case 0x00000000: case 0x56a868a9: obj=A(0); break; /* IUnknown / IGraphBuilder */
    default: break;
    }
    if(!obj){ if(g_trace) fprintf(stderr,"[DShow] QueryInterface refused %08x\n",d1); if(A(2)) wr32(A(2),0); RET(0x80004002u); return; }
    if(A(2)) wr32(A(2),obj); RET(0);
}
static void g_RenderFile(void){
    wide_to_ascii(A(1),g_bgm.path,sizeof g_bgm.path);
    fprintf(stderr,"[DShow] RenderFile \"%s\"\n",g_bgm.path);
    if(is_movie_path(g_bgm.path)){
        if(!movie_open(g_bgm.path)){ RET(VFW_E_NOT_FOUND); return; }   /* the game reports the failure and carries on without a movie */
    } else movie_close();
    RET(0);
}
static void mc_Run(void){
    if(mv.open){
        if(mv.paused) mv.paused=0; else if(!mv.running){ mv.running=1; mv.last_us=0; mv.ended=0; }
        g_bgm.playing=1; RET(0); return;
    }
    g_bgm.playing=1; fprintf(stderr,"[DShow] Run  (%s) vol=%d\n",g_bgm.path,g_bgm.volume); bgm_host_play(g_bgm.path,g_bgm.volume); RET(0);
}
static void mc_Pause(void){ if(mv.open){ mv.paused=1; RET(0); return; } g_bgm.playing=0; RET(0); }
static void mc_Stop(void){
    if(mv.open){ mv.running=0; mv.paused=0; mv.last_us=0; g_bgm.playing=0; dx_movie_audio_clear(); fprintf(stderr,"[DShow] Stop (movie at %.2fs)\n",movie_time()); RET(0); return; }
    g_bgm.playing=0; fprintf(stderr,"[DShow] Stop\n"); bgm_host_stop(); RET(0);
}
static void mc_GetState(void){
    int st=0; if(mv.open) st=mv.running?(mv.paused?1:2):0; else st=g_bgm.playing?2:0;
    if(A(2)) wr32(A(2),st); RET(0);
}
static void wr_double(uint32_t p,double v){ if(p) memcpy(gmem+p,&v,8); }
static void wr_i64(uint32_t p,int64_t v){ if(p) memcpy(gmem+p,&v,8); }
static void mpos_get_Duration(void){ wr_double(A(1),mv.open?mv.duration:0.0); RET(0); }
static void mpos_get_CurrentPosition(void){ wr_double(A(1),mv.open?movie_time():0.0); RET(0); }
static void movie_rewind(void){
    if(!mv.open) return;
    plm_rewind(mv.plm); mv.ended=0; mv.ev_pending=0; mv.last_us=0; mv.rs_pos=0; dx_movie_audio_clear();
}
static void mpos_put_CurrentPosition(void){   /* (this, double) - only "back to the start" is supported */
    double t; memcpy(&t,gmem+cpu.r[ESP]+8,8);
    if(mv.open && t<=0.001) movie_rewind();
    RET(0);
}
static void mev_GetEventHandle(void){ if(A(1)) wr32(A(1),ev_handle); RET(0); }
static void mev_GetEvent(void){     /* (this, long *code, LONG_PTR *p1, LONG_PTR *p2, long timeout) */
    if(mv.ev_pending){ mv.ev_pending=0; if(A(1)) wr32(A(1),EC_COMPLETE); if(A(2)) wr32(A(2),0); if(A(3)) wr32(A(3),0); RET(0); return; }
    RET(E_ABORT_);                  /* no event pending */
}
static void mev_Wait(void){         /* (this, long timeout, long *code) */
    if(mv.ended){ if(A(2)) wr32(A(2),EC_COMPLETE); RET(0); return; }
    if(A(2)) wr32(A(2),0); RET(E_ABORT_);
}
static void mev_SetNotifyWindow(void){ mv.notify_hwnd=A(1); mv.notify_msg=A(2); mv.notify_lparam=A(3); RET(0); }
static void aud_put_Volume(void){
    g_bgm.volume=(int)A(1); int v=g_bgm.volume; mv.gain=(v<=-10000)?0.0:pow(10.0,(v>0?0:v)/2000.0); RET(0);
}
static void aud_get_Volume(void){ if(A(1)) wr32(A(1),(uint32_t)g_bgm.volume); RET(0); }
/* IMediaSeeking (100 ns units) */
static const uint8_t TIME_FORMAT_MEDIA_TIME[16]={0x74,0x55,0x78,0x7B,0x82,0x8C,0xcf,0x11,0xBC,0x0C,0x00,0xAA,0x00,0xAC,0x74,0xF6};
static void sk_GetCapabilities(void){ if(A(1)) wr32(A(1),0x1|0x2|0x4|0x10|0x20|0x40); RET(0); }   /* CanSeekAbsolute/Forwards/Backwards, GetCurrentPos/StopPos/Duration */
static void sk_GetTimeFormat(void){ if(A(1)) memcpy(gmem+A(1),TIME_FORMAT_MEDIA_TIME,16); RET(0); }
static void sk_IsUsingTimeFormat(void){ RET(0); }   /* S_OK: it is MEDIA_TIME */
static void sk_GetDuration(void){ wr_i64(A(1),mv.open?(int64_t)(mv.duration*1e7):0); RET(0); }
static void sk_GetCurrentPosition(void){ wr_i64(A(1),mv.open?(int64_t)(movie_time()*1e7):0); RET(0); }
static void sk_GetPositions(void){ wr_i64(A(1),mv.open?(int64_t)(movie_time()*1e7):0); wr_i64(A(2),mv.open?(int64_t)(mv.duration*1e7):0); RET(0); }
static void sk_SetPositions(void){   /* (this, LONGLONG *cur, DWORD curFlags, LONGLONG *stop, DWORD stopFlags) - only a rewind to 0 is supported */
    if(mv.open && A(1) && (rd32(A(2))&7)==1 /*AM_SEEKING_AbsolutePositioning*/){
        int64_t t; memcpy(&t,gmem+A(1),8); if(t<=0) movie_rewind();
    }
    RET(0);
}
/* IBasicVideo */
static void bv_AvgTimePerFrame(void){ wr_double(A(1),mv.open?1.0/plm_get_framerate(mv.plm):0.0); RET(0); }
static void bv_get_VideoWidth(void){ if(A(1)) wr32(A(1),(uint32_t)mv.w); RET(0); }
static void bv_get_VideoHeight(void){ if(A(1)) wr32(A(1),(uint32_t)mv.h); RET(0); }
static void bv_GetVideoSize(void){ if(A(1)) wr32(A(1),(uint32_t)mv.w); if(A(2)) wr32(A(2),(uint32_t)mv.h); RET(0); }
static void bv_GetRect(void){ if(A(1)) wr32(A(1),0); if(A(2)) wr32(A(2),0); if(A(3)) wr32(A(3),(uint32_t)mv.w); if(A(4)) wr32(A(4),(uint32_t)mv.h); RET(0); }

#define MD(n,a) {n,a}
static const MDef graph_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("AddFilter",3),MD("RemoveFilter",2),MD("EnumFilters",2),MD("FindFilterByName",3),MD("ConnectDirect",4),MD("Reconnect",2),MD("Disconnect",2),MD("SetDefaultSyncSource",1),MD("Connect",3),MD("Render",2),MD("RenderFile",3),MD("AddSourceFilter",4),MD("SetLogFile",2),MD("Abort",1),MD("ShouldOperationContinue",1)};
static const MDef mc_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),MD("Run",1),MD("Pause",1),MD("Stop",1),MD("GetState",3),MD("RenderFile",2),MD("AddSourceFilter",3),MD("get_FilterCollection",2),MD("get_RegFilterCollection",2),MD("StopWhenReady",1)};
/* IMediaEvent derives from IDispatch, so its methods start at slot 7 */
static const MDef mev_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),MD("GetEventHandle",2),MD("GetEvent",5),MD("WaitForCompletion",3),MD("CancelDefaultHandling",2),MD("RestoreDefaultHandling",2),MD("FreeEventParams",4),MD("SetNotifyWindow",4),MD("SetNotifyFlags",2),MD("GetNotifyFlags",2)};
static const MDef mpos_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),MD("get_Duration",2),MD("put_CurrentPosition",3),MD("get_CurrentPosition",2),MD("get_StopTime",2),MD("put_StopTime",3),MD("get_PrerollTime",2),MD("put_PrerollTime",3),MD("put_Rate",3),MD("get_Rate",2),MD("CanSeekForward",2),MD("CanSeekBackward",2)};
static const MDef aud_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),MD("put_Volume",2),MD("get_Volume",2),MD("put_Balance",2),MD("get_Balance",2)};
static const MDef seek_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetCapabilities",2),MD("CheckCapabilities",2),MD("IsFormatSupported",2),MD("QueryPreferredFormat",2),MD("GetTimeFormat",2),MD("IsUsingTimeFormat",2),MD("SetTimeFormat",2),MD("GetDuration",2),MD("GetStopPosition",2),MD("GetCurrentPosition",2),MD("ConvertTimeFormat",6),MD("SetPositions",5),MD("GetPositions",3),MD("GetAvailable",3),MD("SetRate",3),MD("GetRate",2),MD("GetPreroll",2)};
static const MDef bv_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),MD("get_AvgTimePerFrame",2),MD("get_BitRate",2),MD("get_BitErrorRate",2),MD("get_VideoWidth",2),MD("get_VideoHeight",2),
    MD("put_SourceLeft",2),MD("get_SourceLeft",2),MD("put_SourceWidth",2),MD("get_SourceWidth",2),MD("put_SourceTop",2),MD("get_SourceTop",2),MD("put_SourceHeight",2),MD("get_SourceHeight",2),
    MD("put_DestinationLeft",2),MD("get_DestinationLeft",2),MD("put_DestinationWidth",2),MD("get_DestinationWidth",2),MD("put_DestinationTop",2),MD("get_DestinationTop",2),MD("put_DestinationHeight",2),MD("get_DestinationHeight",2),
    MD("SetSourcePosition",5),MD("GetSourcePosition",5),MD("SetDefaultSourcePosition",1),MD("SetDestinationPosition",5),MD("GetDestinationPosition",5),MD("SetDefaultDestinationPosition",1),MD("GetVideoSize",3),MD("GetVideoPaletteEntries",5),MD("GetCurrentImage",3),MD("IsUsingDefaultSource",1),MD("IsUsingDefaultDestination",1)};
/* IVideoWindow has ~40 methods; all return S_OK. Arg counts per Quartz control.h (this + params; BSTR/long/pointer = 1, OAHWND/OAFilterState = 1). */
static const MDef vw_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),
    MD("put_Caption",2),MD("get_Caption",2),MD("put_WindowStyle",2),MD("get_WindowStyle",2),MD("put_WindowStyleEx",2),MD("get_WindowStyleEx",2),MD("put_AutoShow",2),MD("get_AutoShow",2),
    MD("put_WindowState",2),MD("get_WindowState",2),MD("put_BackgroundPalette",2),MD("get_BackgroundPalette",2),MD("put_Visible",2),MD("get_Visible",2),MD("put_Left",2),MD("get_Left",2),
    MD("put_Width",2),MD("get_Width",2),MD("put_Top",2),MD("get_Top",2),MD("put_Height",2),MD("get_Height",2),MD("put_Owner",2),MD("get_Owner",2),MD("put_MessageDrain",2),MD("get_MessageDrain",2),
    MD("get_BorderColor",2),MD("put_BorderColor",2),MD("get_FullScreenMode",2),MD("put_FullScreenMode",2),MD("SetWindowForeground",2),MD("NotifyOwnerMessage",5),MD("SetWindowPosition",5),
    MD("GetWindowPosition",5),MD("GetMinIdealImageSize",3),MD("GetMaxIdealImageSize",3),MD("GetRestorePosition",5),MD("HideCursor",2),MD("IsCursorHidden",2)};
#define CNT(a) ((int)(sizeof(a)/sizeof((a)[0])))
static hle_fn graph_impl[32], mc_impl[32], mev_impl[32], mpos_impl[32], aud_impl[32], vw_impl[64], seek_impl[32], bv_impl[48];

uint32_t dshow_make_graph(void){
    if(!graph_obj){
        for(int i=0;i<32;i++){ graph_impl[i]=mc_impl[i]=mev_impl[i]=mpos_impl[i]=aud_impl[i]=seek_impl[i]=d_zero; } for(int i=0;i<64;i++) vw_impl[i]=d_zero; for(int i=0;i<48;i++) bv_impl[i]=d_zero;
        graph_impl[0]=mc_impl[0]=mev_impl[0]=mpos_impl[0]=aud_impl[0]=vw_impl[0]=seek_impl[0]=bv_impl[0]=d_qi;
        graph_impl[1]=mc_impl[1]=mev_impl[1]=mpos_impl[1]=aud_impl[1]=vw_impl[1]=seek_impl[1]=bv_impl[1]=d_addref;
        graph_impl[2]=mc_impl[2]=mev_impl[2]=mpos_impl[2]=aud_impl[2]=vw_impl[2]=seek_impl[2]=bv_impl[2]=d_release;
        graph_impl[13]=g_RenderFile; mc_impl[7]=mc_Run; mc_impl[8]=mc_Pause; mc_impl[9]=mc_Stop; mc_impl[10]=mc_GetState;
        mev_impl[7]=mev_GetEventHandle; mev_impl[8]=mev_GetEvent; mev_impl[9]=mev_Wait; mev_impl[13]=mev_SetNotifyWindow;
        mpos_impl[7]=mpos_get_Duration; mpos_impl[8]=mpos_put_CurrentPosition; mpos_impl[9]=mpos_get_CurrentPosition;
        aud_impl[7]=aud_put_Volume; aud_impl[8]=aud_get_Volume;
        seek_impl[3]=sk_GetCapabilities; seek_impl[7]=sk_GetTimeFormat; seek_impl[8]=sk_IsUsingTimeFormat; seek_impl[10]=sk_GetDuration; seek_impl[11]=sk_GetDuration;
        seek_impl[12]=sk_GetCurrentPosition; seek_impl[14]=sk_SetPositions; seek_impl[15]=sk_GetPositions;
        bv_impl[7]=bv_AvgTimePerFrame; bv_impl[10]=bv_get_VideoWidth; bv_impl[11]=bv_get_VideoHeight; bv_impl[29]=bv_GetRect; bv_impl[32]=bv_GetRect;   /* Get{Source,Destination}Position (out pointers); the Set* ones take values */ bv_impl[34]=bv_GetVideoSize;
        static Iface g={"IGraphBuilder",0,0,0}, mc={"IMediaControl",0,0,0}, me={"IMediaEventEx",0,0,0}, mp={"IMediaPosition",0,0,0}, au={"IBasicAudio",0,0,0}, vw={"IVideoWindow",0,0,0}, sk={"IMediaSeeking",0,0,0}, bvi={"IBasicVideo",0,0,0};
        g.n=CNT(graph_m); g.m=graph_m; g.impl=graph_impl; mc.n=CNT(mc_m); mc.m=mc_m; mc.impl=mc_impl; me.n=CNT(mev_m); me.m=mev_m; me.impl=mev_impl;
        mp.n=CNT(mpos_m); mp.m=mpos_m; mp.impl=mpos_impl; au.n=CNT(aud_m); au.m=aud_m; au.impl=aud_impl; vw.n=CNT(vw_m); vw.m=vw_m; vw.impl=vw_impl;
        sk.n=CNT(seek_m); sk.m=seek_m; sk.impl=seek_impl; bvi.n=CNT(bv_m); bvi.m=bv_m; bvi.impl=bv_impl;
        vt_graph=make_vtable(&g); vt_mc=make_vtable(&mc); vt_mev=make_vtable(&me); vt_mpos=make_vtable(&mp); vt_aud=make_vtable(&au); vt_vw=make_vtable(&vw); vt_seek=make_vtable(&sk); vt_bv=make_vtable(&bvi);
        graph_obj=ga_alloc(8,1); wr32(graph_obj,vt_graph); mc_obj=ga_alloc(8,1); wr32(mc_obj,vt_mc); mev_obj=ga_alloc(8,1); wr32(mev_obj,vt_mev);
        mpos_obj=ga_alloc(8,1); wr32(mpos_obj,vt_mpos); aud_obj=ga_alloc(8,1); wr32(aud_obj,vt_aud); vw_obj=ga_alloc(8,1); wr32(vw_obj,vt_vw);
        seek_obj=ga_alloc(8,1); wr32(seek_obj,vt_seek); bv_obj=ga_alloc(8,1); wr32(bv_obj,vt_bv);
        extern uint32_t hle_make_event(void); ev_handle=hle_make_event();
    }
    return graph_obj;
}
