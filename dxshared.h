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
} Surf;
uint32_t make_vtable(Iface *ifc);
void com_default(void);
void com_addref(void);
void com_release(void);
Surf *surf_of(uint32_t obj);
uint32_t d3d_get_object(void);
void d3d_register(void);
