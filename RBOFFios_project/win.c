#include "hle.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#define MAXCLS 16
static struct { char name[64]; uint32_t proc; } cls[MAXCLS]; static int ncls=0;
static uint32_t main_hwnd=0, main_proc=0; static int win_w=640, win_h=480; static int activated=0;

static uint32_t send_msg(uint32_t proc,uint32_t hwnd,uint32_t msg,uint32_t w,uint32_t l){
    uint32_t a[4]={hwnd,msg,w,l}; return guest_call(proc,4,a);
}
static void h_RegisterClassA(void){
    uint32_t p=A(0); if(ncls<MAXCLS){ cls[ncls].proc=rd32(p+4); snprintf(cls[ncls].name,64,"%s",(char*)gmem+rd32(p+36)); if(g_trace)fprintf(stderr,"[RegisterClass] %s proc=%08x\n",cls[ncls].name,cls[ncls].proc); ncls++; }
    RET(0xC000+ncls);
}
static void h_CreateWindowExA(void){
    const char *cn=(const char*)gmem+A(1); uint32_t proc=0;
    for(int i=0;i<ncls;i++) if(!strcasecmp(cls[i].name,cn)) proc=cls[i].proc;
    int w=(int)A(6), h=(int)A(7);
    if(g_trace) fprintf(stderr,"[CreateWindowExA] class=%s title=%s style=%08x %dx%d proc=%08x\n",cn,(char*)gmem+A(2),A(3),w,h,proc);
    uint32_t hwnd=0x10000+ (main_hwnd? 4:0);
    if(!main_hwnd){ main_hwnd=hwnd; main_proc=proc; if(w>0)win_w=w; if(h>0)win_h=h; }
    if(proc){
        uint32_t cs=ga_alloc(64,1); wr32(cs,A(11)); wr32(cs+4,A(10));
        send_msg(proc,hwnd,0x81 /*WM_NCCREATE*/,0,cs);
        send_msg(proc,hwnd,0x01 /*WM_CREATE*/,0,cs);
    }
    RET(hwnd);
}
static void activate(void){
    if(activated||!main_proc) return; activated=1;
    send_msg(main_proc,main_hwnd,0x18,1,0);
    send_msg(main_proc,main_hwnd,0x1C,1,0);
    send_msg(main_proc,main_hwnd,0x06,1,0);
    send_msg(main_proc,main_hwnd,0x07,0,0);
}
static void h_ShowWindow(void){ activate(); RET(1); }
static void h_SetFocus(void){ activate(); RET(A(0)); }
static void h_SetForegroundWindow(void){ activate(); RET(1); }
static void h_UpdateWindow(void){ RET(1); }
static void h_DefWindowProcA(void){ RET(0); }
static int cursor_count=0;
static void h_ShowCursor(void){ cursor_count += A(0)?1:-1; RET(cursor_count); }
static void h_zero(void){ RET(0); }
static void h_one(void){ RET(1); }
static void h_RectClient(void){ uint32_t r=A(1); wr32(r,0); wr32(r+4,0); wr32(r+8,win_w); wr32(r+12,win_h); RET(1); }
static void h_GetSystemMetrics(void){ switch(A(0)){ case 0: RET(1920); break; case 1: RET(1200); break; default: RET(0);} }
static void h_SetRect(void){ uint32_t r=A(0); wr32(r,A(1)); wr32(r+4,A(2)); wr32(r+8,A(3)); wr32(r+12,A(4)); RET(1); }
static void h_GetCursorPos(void){ wr32(A(0),320); wr32(A(0)+4,240); RET(1); }
static void h_ClientToScreen(void){ RET(1); }
static void h_MessageBoxA(void){ fprintf(stderr,"[MessageBoxA] %s: %s\n",(char*)gmem+A(2),(char*)gmem+A(1)); RET(1); }
static void h_DialogBoxParamA(void){ fprintf(stderr,"[DialogBoxParamA] template=%08x (returning IDOK)\n",A(1)); RET(1); }
typedef struct { int active; uint32_t hwnd, id, proc; uint64_t interval_us, next_us; } Timer;
static Timer timers[16];
static void h_SetTimer(void){
    uint32_t hwnd=A(0), id=A(1), ms=A(2), proc=A(3); int slot=-1;
    for(int i=0;i<16;i++) if(timers[i].active&&timers[i].hwnd==hwnd&&timers[i].id==id){ slot=i; break; }
    if(slot<0) for(int i=0;i<16;i++) if(!timers[i].active){ slot=i; break; }
    if(slot<0){ RET(0); return; }
    if(!id) id=0x100+slot;
    Timer *t=&timers[slot]; t->active=1; t->hwnd=hwnd; t->id=id; t->proc=proc; t->interval_us=(uint64_t)(ms?ms:1)*1000ull; t->next_us=hle_now_us()+t->interval_us;
    if(g_trace) fprintf(stderr,"[SetTimer] id=%u %ums proc=%08x\n",id,ms,proc);
    RET(id);
}
static void h_KillTimer(void){ for(int i=0;i<16;i++) if(timers[i].active&&timers[i].hwnd==A(0)&&timers[i].id==A(1)) timers[i].active=0; RET(1); }
/* posted messages (DirectShow's WM_GRAPHNOTIFY etc.); delivered by PeekMessage ahead of timers */
static struct { uint32_t hwnd,msg,w,l; } pq[16]; static int pq_n=0;
void win_post_message(uint32_t hwnd,uint32_t msg,uint32_t wp,uint32_t lp){ if(pq_n<16){ pq[pq_n].hwnd=hwnd; pq[pq_n].msg=msg; pq[pq_n].w=wp; pq[pq_n].l=lp; pq_n++; } }
void dshow_movie_pump(void);
static void h_PeekMessageA(void){
    dshow_movie_pump();
    uint64_t now=hle_now_us(); uint32_t msg=A(0);
    if(pq_n>0){
        wr32(msg,pq[0].hwnd); wr32(msg+4,pq[0].msg); wr32(msg+8,pq[0].w); wr32(msg+12,pq[0].l); wr32(msg+16,(uint32_t)(now/1000)); wr32(msg+20,0); wr32(msg+24,0);
        pq_n--; for(int i=0;i<pq_n;i++) pq[i]=pq[i+1];
        RET(1); return;
    }
    for(int i=0;i<16;i++) if(timers[i].active&&now>=timers[i].next_us){
        Timer *t=&timers[i]; t->next_us=now+t->interval_us;   /* coalesce missed ticks like Windows */
        wr32(msg,t->hwnd); wr32(msg+4,0x113); wr32(msg+8,t->id); wr32(msg+12,t->proc); wr32(msg+16,(uint32_t)(now/1000)); wr32(msg+20,0); wr32(msg+24,0);
        RET(1); return;
    }
    RET(0);
}
static void h_DispatchMessageA(void){
    uint32_t msg=A(0), hwnd=rd32(msg), m=rd32(msg+4), wp=rd32(msg+8), lp=rd32(msg+12);
    if(m==0x113 && lp){ uint32_t a[4]={hwnd,0x113,wp,rd32(msg+16)}; guest_call(lp,4,a); RET(0); return; }   /* TIMERPROC */
    if(main_proc){ RET(send_msg(main_proc,hwnd?hwnd:main_hwnd,m,wp,lp)); return; }
    RET(0);
}
static void h_GetMessageA(void){ fprintf(stderr,"[GetMessageA] -> WM_QUIT\n"); RET(0); }
static void h_ImmGetDefaultIMEWnd(void){ RET(0); }
uint32_t dshow_make_graph(void);
static void h_CoCreateInstance(void){
    uint32_t clsid=A(0); uint32_t d1=clsid?rd32(clsid):0;
    if(d1==0xe436ebb3u){ if(A(4)) wr32(A(4),dshow_make_graph()); RET(0); return; }   /* CLSID_FilterGraph */
    fprintf(stderr,"[CoCreateInstance] unsupported clsid %08x\n",d1);
    if(A(4)) wr32(A(4),0); RET(0x80040154u);
}
static void h_PathRenameExtensionA(void){
    uint32_t p=A(0); char *s=(char*)gmem+p; char *dot=strrchr(s,'.'); char *sl=strrchr(s,'\\');
    if(!dot||(sl&&dot<sl)) dot=s+strlen(s);
    strcpy(dot,(char*)gmem+A(1)); RET(1);
}
static void h_GetSystemPaletteEntries(void){ RET(0); }
static void h_TranslateMessage(void){ RET(0); }

static void h_PathCombineA(void){
    char *d=(char*)gmem+A(0); const char *dir=A(1)?(char*)gmem+A(1):"", *f=A(2)?(char*)gmem+A(2):"";
    char tmp[600];
    if(f[0]=='\\'||(f[0]&&f[1]==':')) snprintf(tmp,sizeof tmp,"%s",f);
    else if(!dir[0]) snprintf(tmp,sizeof tmp,"%s",f);
    else { size_t l=strlen(dir); snprintf(tmp,sizeof tmp,"%s%s%s",dir,(dir[l-1]=='\\'||dir[l-1]=='/')?"":"\\",f); }
    strcpy(d,tmp); RET(A(0));
}
static void h_PathFileExistsA(void){ char host[1200]; RET(resolve_path((char*)gmem+A(0),host,sizeof host)?1:0); }
static void h_SHBrowseForFolderA(void){ fprintf(stderr,"[SHBrowseForFolderA] -> game folder\n"); RET(ga_alloc(16,1)); }
static void h_SHGetPathFromIDListA(void){ strcpy((char*)gmem+A(1),".\\"); RET(1); }
static void h_RegFail(void){ RET(2); }
static void h_RegClose(void){ RET(0); }

#define R(dll,name,n,fn) hle_register(dll,name,n,fn)
void win_register(void){
    const char *U="USER32.dll", *G="GDI32.dll", *I="IMM32.dll";
    R(U,"RegisterClassA",1,h_RegisterClassA); R(U,"CreateWindowExA",12,h_CreateWindowExA);
    R(U,"ShowWindow",2,h_ShowWindow); R(U,"SetFocus",1,h_SetFocus); R(U,"SetForegroundWindow",1,h_SetForegroundWindow);
    R(U,"UpdateWindow",1,h_UpdateWindow); R(U,"DefWindowProcA",4,h_DefWindowProcA);
    R(U,"GetClientRect",2,h_RectClient); R(U,"GetWindowRect",2,h_RectClient);
    R(U,"GetSystemMetrics",1,h_GetSystemMetrics); R(U,"SetRect",5,h_SetRect); R(U,"GetCursorPos",1,h_GetCursorPos);
    R(U,"ClientToScreen",2,h_ClientToScreen); R(U,"ScreenToClient",2,h_ClientToScreen);
    R(U,"MessageBoxA",4,h_MessageBoxA); R(U,"DialogBoxParamA",5,h_DialogBoxParamA);
    R(U,"PeekMessageA",5,h_PeekMessageA); R(U,"GetMessageA",4,h_GetMessageA); R(U,"TranslateMessage",1,h_TranslateMessage);
    R(U,"DispatchMessageA",1,h_DispatchMessageA); R(U,"PostQuitMessage",1,h_zero);
    R(U,"GetDC",1,h_one); R(U,"ReleaseDC",2,h_one); R(U,"SetWindowLongA",3,h_zero);
    R(U,"AdjustWindowRect",3,h_one); R(U,"AdjustWindowRectEx",4,h_one); R(U,"GetDesktopWindow",0,h_one);
    R(U,"MoveWindow",6,h_one); R(U,"GetActiveWindow",0,h_one); R(U,"ShowCursor",1,h_ShowCursor);
    R(U,"LoadCursorA",2,h_one); R(U,"LoadIconA",2,h_one); R(U,"SetWindowPos",7,h_one);
    R(U,"GetDlgItem",2,h_one); R(U,"SendDlgItemMessageA",5,h_zero); R(U,"EndDialog",2,h_one);
    R(U,"SendMessageA",4,h_zero); R(U,"KillTimer",2,h_KillTimer); R(U,"SetTimer",4,h_SetTimer);
    R(U,"SetDlgItemTextA",3,h_one); R(U,"InvalidateRect",3,h_one); R(U,"DestroyWindow",1,h_one);
    R(G,"TextOutA",5,h_one); R(G,"GetSystemPaletteEntries",4,h_GetSystemPaletteEntries); R(G,"GetStockObject",1,h_one);
    R(G,"PatBlt",6,h_one); R(G,"BitBlt",9,h_one);
    R(I,"ImmGetContext",1,h_zero); R(I,"ImmSetOpenStatus",2,h_one); R(I,"ImmAssociateContext",2,h_zero);
    R(I,"ImmGetDefaultIMEWnd",1,h_ImmGetDefaultIMEWnd); R(I,"ImmReleaseContext",2,h_one);
    R("ole32.dll","CoInitialize",1,h_zero); R("ole32.dll","CoCreateInstance",5,h_CoCreateInstance); R("ole32.dll","CoUninitialize",0,h_zero);
    R("SHLWAPI.dll","PathRenameExtensionA",2,h_PathRenameExtensionA);
    R("SHLWAPI.dll","PathCombineA",3,h_PathCombineA); R("SHLWAPI.dll","PathFileExistsA",1,h_PathFileExistsA);
    R("SHELL32.dll","SHBrowseForFolderA",1,h_SHBrowseForFolderA); R("SHELL32.dll","SHGetPathFromIDListA",2,h_SHGetPathFromIDListA);
    R("USER32.dll","CreateDialogParamA",5,h_one);
    R("ADVAPI32.dll","RegOpenKeyExA",5,h_RegFail); R("ADVAPI32.dll","RegQueryValueExA",6,h_RegFail); R("ADVAPI32.dll","RegCloseKey",1,h_RegClose);
    R("ole32.dll","CoTaskMemFree",1,h_zero); R("WINMM.dll","timeBeginPeriod",1,h_zero);
}
