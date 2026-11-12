/* Minimal DirectShow filter-graph emulation: enough for "RenderFile + Run/Stop/Position/Volume" music playback. */
#include "dxshared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int playing; char path[512]; int volume; double pos; } Bgm;
Bgm g_bgm;
static uint32_t vt_graph, vt_mc, vt_mev, vt_mpos, vt_aud, vt_vw, vt_seek;
static uint32_t graph_obj=0, mc_obj=0, mev_obj=0, mpos_obj=0, aud_obj=0, vw_obj=0;
static uint32_t ev_handle=0;
extern int g_cur_idx;
void bgm_host_play(const char *path,int volume);   /* platform hooks (weak defaults in main.c) */
void bgm_host_stop(void);

static void wide_to_ascii(uint32_t w,char *out,int cap){ int i=0; for(;i<cap-1;i++){ uint16_t c=rd16(w+2*i); if(!c)break; out[i]=(c<128)?(char)c:'?'; } out[i]=0; }
static void trace_call(void){ if(g_trace) fprintf(stderr,"[DShow] %s\n",hle_name(g_cur_idx)); }

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
    case 0x00000000: case 0x56a868a9: obj=A(0); break; /* IUnknown / IGraphBuilder */
    default: break;
    }
    if(!obj){ if(g_trace) fprintf(stderr,"[DShow] QueryInterface refused %08x\n",d1); if(A(2)) wr32(A(2),0); RET(0x80004002u); return; }
    if(A(2)) wr32(A(2),obj); RET(0);
}
static void g_RenderFile(void){
    wide_to_ascii(A(1),g_bgm.path,sizeof g_bgm.path);
    fprintf(stderr,"[DShow] RenderFile \"%s\"\n",g_bgm.path); RET(0);
}
static void mc_Run(void){ g_bgm.playing=1; fprintf(stderr,"[DShow] Run  (%s) vol=%d\n",g_bgm.path,g_bgm.volume); bgm_host_play(g_bgm.path,g_bgm.volume); RET(0); }
static void mc_Pause(void){ g_bgm.playing=0; RET(0); }
static void mc_Stop(void){ g_bgm.playing=0; fprintf(stderr,"[DShow] Stop\n"); bgm_host_stop(); RET(0); }
static void mc_GetState(void){ if(A(2)) wr32(A(2),g_bgm.playing?2:0); RET(0); }
static void mpos_getdouble(void){ if(A(1)){ memset(gmem+A(1),0,8); } RET(0); }
static void aud_put_Volume(void){ g_bgm.volume=(int)A(1); RET(0); }
static void aud_get_Volume(void){ if(A(1)) wr32(A(1),(uint32_t)g_bgm.volume); RET(0); }
static void mev_GetEventHandle(void){ if(A(1)) wr32(A(1),ev_handle); RET(0); }
static void mev_GetEvent(void){ RET(0x80004004u); }                 /* E_ABORT: no event pending */
static void mev_Wait(void){ RET(0x80004004u); }

#define MD(n,a) {n,a}
static const MDef graph_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("AddFilter",3),MD("RemoveFilter",2),MD("EnumFilters",2),MD("FindFilterByName",3),MD("ConnectDirect",4),MD("Reconnect",2),MD("Disconnect",2),MD("SetDefaultSyncSource",1),MD("Connect",3),MD("Render",2),MD("RenderFile",3),MD("AddSourceFilter",4),MD("SetLogFile",2),MD("Abort",1),MD("ShouldOperationContinue",1)};
static const MDef mc_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),MD("Run",1),MD("Pause",1),MD("Stop",1),MD("GetState",3),MD("RenderFile",2),MD("AddSourceFilter",3),MD("get_FilterCollection",2),MD("get_RegFilterCollection",2),MD("StopWhenReady",1)};
static const MDef mev_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetEventHandle",2),MD("GetEvent",6),MD("WaitForCompletion",3),MD("CancelDefaultHandling",2),MD("RestoreDefaultHandling",2),MD("FreeEventParams",4),MD("SetNotifyWindow",4),MD("SetNotifyFlags",2),MD("GetNotifyFlags",2)};
static const MDef mpos_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),MD("get_Duration",2),MD("put_CurrentPosition",3),MD("get_CurrentPosition",2),MD("get_StopTime",2),MD("put_StopTime",3),MD("get_PrerollTime",2),MD("put_PrerollTime",3),MD("put_Rate",3),MD("get_Rate",2),MD("CanSeekForward",2),MD("CanSeekBackward",2)};
static const MDef aud_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),MD("put_Volume",2),MD("get_Volume",2),MD("put_Balance",2),MD("get_Balance",2)};
/* IVideoWindow has ~40 methods; all return S_OK. Arg counts per Quartz control.h (this + params; BSTR/long/pointer = 1, OAHWND/OAFilterState = 1). */
static const MDef vw_m[]={MD("QueryInterface",3),MD("AddRef",1),MD("Release",1),MD("GetTypeInfoCount",2),MD("GetTypeInfo",4),MD("GetIDsOfNames",6),MD("Invoke",8),
    MD("put_Caption",2),MD("get_Caption",2),MD("put_WindowStyle",2),MD("get_WindowStyle",2),MD("put_WindowStyleEx",2),MD("get_WindowStyleEx",2),MD("put_AutoShow",2),MD("get_AutoShow",2),
    MD("put_WindowState",2),MD("get_WindowState",2),MD("put_BackgroundPalette",2),MD("get_BackgroundPalette",2),MD("put_Visible",2),MD("get_Visible",2),MD("put_Left",2),MD("get_Left",2),
    MD("put_Width",2),MD("get_Width",2),MD("put_Top",2),MD("get_Top",2),MD("put_Height",2),MD("get_Height",2),MD("put_Owner",2),MD("get_Owner",2),MD("put_MessageDrain",2),MD("get_MessageDrain",2),
    MD("get_BorderColor",2),MD("put_BorderColor",2),MD("get_FullScreenMode",2),MD("put_FullScreenMode",2),MD("SetWindowForeground",2),MD("NotifyOwnerMessage",5),MD("SetWindowPosition",5),
    MD("GetWindowPosition",5),MD("GetMinIdealImageSize",3),MD("GetMaxIdealImageSize",3),MD("GetRestorePosition",5),MD("HideCursor",2),MD("IsCursorHidden",2)};
#define CNT(a) ((int)(sizeof(a)/sizeof((a)[0])))
static hle_fn graph_impl[32], mc_impl[32], mev_impl[32], mpos_impl[32], aud_impl[32], vw_impl[64];

uint32_t dshow_make_graph(void){
    if(!graph_obj){
        for(int i=0;i<32;i++){ graph_impl[i]=mc_impl[i]=mev_impl[i]=mpos_impl[i]=aud_impl[i]=d_zero; } for(int i=0;i<64;i++) vw_impl[i]=d_zero;
        graph_impl[0]=mc_impl[0]=mev_impl[0]=mpos_impl[0]=aud_impl[0]=vw_impl[0]=d_qi;
        graph_impl[1]=mc_impl[1]=mev_impl[1]=mpos_impl[1]=aud_impl[1]=vw_impl[1]=d_addref;
        graph_impl[2]=mc_impl[2]=mev_impl[2]=mpos_impl[2]=aud_impl[2]=vw_impl[2]=d_release;
        graph_impl[13]=g_RenderFile; mc_impl[7]=mc_Run; mc_impl[8]=mc_Pause; mc_impl[9]=mc_Stop; mc_impl[10]=mc_GetState;
        mev_impl[3]=mev_GetEventHandle; mev_impl[4]=mev_GetEvent; mev_impl[5]=mev_Wait;
        mpos_impl[7]=mpos_getdouble; mpos_impl[9]=mpos_getdouble; aud_impl[7]=aud_put_Volume; aud_impl[8]=aud_get_Volume;
        static Iface g={"IGraphBuilder",0,0,0}, mc={"IMediaControl",0,0,0}, me={"IMediaEventEx",0,0,0}, mp={"IMediaPosition",0,0,0}, au={"IBasicAudio",0,0,0}, vw={"IVideoWindow",0,0,0};
        g.n=CNT(graph_m); g.m=graph_m; g.impl=graph_impl; mc.n=CNT(mc_m); mc.m=mc_m; mc.impl=mc_impl; me.n=CNT(mev_m); me.m=mev_m; me.impl=mev_impl;
        mp.n=CNT(mpos_m); mp.m=mpos_m; mp.impl=mpos_impl; au.n=CNT(aud_m); au.m=aud_m; au.impl=aud_impl; vw.n=CNT(vw_m); vw.m=vw_m; vw.impl=vw_impl;
        vt_graph=make_vtable(&g); vt_mc=make_vtable(&mc); vt_mev=make_vtable(&me); vt_mpos=make_vtable(&mp); vt_aud=make_vtable(&au); vt_vw=make_vtable(&vw);
        graph_obj=ga_alloc(8,1); wr32(graph_obj,vt_graph); mc_obj=ga_alloc(8,1); wr32(mc_obj,vt_mc); mev_obj=ga_alloc(8,1); wr32(mev_obj,vt_mev);
        mpos_obj=ga_alloc(8,1); wr32(mpos_obj,vt_mpos); aud_obj=ga_alloc(8,1); wr32(aud_obj,vt_aud); vw_obj=ga_alloc(8,1); wr32(vw_obj,vt_vw);
        extern uint32_t hle_make_event(void); ev_handle=hle_make_event();
    }
    return graph_obj;
}
