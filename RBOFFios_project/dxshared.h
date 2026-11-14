#pragma once
#include "hle.h"
typedef struct { const char *name; int nargs; } MDef;
typedef struct { const char *iface; int n; const MDef *m; hle_fn *impl; } Iface;
typedef struct {
    int used, w,h,bpp,pitch; uint32_t pix, obj;
    uint32_t rmask,gmask,bmask,amask;
    int is_primary, is_back; int back_id;
    int ck_valid; uint32_t ck_lo, ck_hi;
    int locked;
    uint32_t *cache; uint8_t *keymask; int cache_ok; int cache_cap;
    int id; uint32_t cgen, ggen, ggen_k;   /* surface index; texture-cache generation / generation last uploaded to the GPU (gpu_raster) */
} Surf;
uint32_t make_vtable(Iface *ifc);
void com_default(void);
void com_addref(void);
void com_release(void);
Surf *surf_of(uint32_t obj);
uint32_t d3d_get_object(void);
void d3d_register(void);
/* GPU rasterizer coherence (d3d.c): the render target may live on the GPU; call before the CPU reads (write=0) or writes (write=1) a surface's pixels */
void gpu_surf_access(Surf *s,int write);
void gpu_surf_release(Surf *s);     /* surface slot is being recycled */
int  d3d_gpu_report(char *out,int n);
