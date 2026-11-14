#pragma once
/* GPU (Metal) triangle rasterizer used by d3d.c in place of the CPU rasterizer for big scenes.
 *
 * The emulator keeps every DirectDraw surface in guest memory (gmem).  The GPU path keeps ONE render target resident as a texture:
 *   d3d.c uploads it (gpu_rt_upload), appends triangles (gpu_draw) and reads it back (gpu_rt_read) only when the CPU needs the pixels
 *   again (Blt/Lock/present).  Everything here is called from the emulator thread only.
 *
 * Vertex format passed to gpu_draw: 8 floats per vertex = { x, y, u, v, r, g, b, a }  (x,y in render-target pixels, colour 0..1).
 * Three consecutive vertices form one triangle (already culled / wound by the caller). */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct GpuState {
    int tex_on, cop, aop, ca1, ca2, aa1, aa2;   /* texture stage 0 (D3DTSS colour/alpha op and args), see d3d.c */
    int use_key;                                /* colour-keyed texture (kill texels flagged in the key mask) */
    int atest, afunc, aref;                     /* alpha test */
    int point;                                  /* 1 = point sampling (with the CPU rasterizer's tie bias), 0 = bilinear */
    int addr;                                   /* 0 wrap, 1 mirror, 2 clamp */
    int blend, sf, df;                          /* D3D blend enable / src / dest factor */
    int write_alpha;                            /* render target has an alpha channel */
    int tex_id;                                 /* texture slot (surface id, +512 for the colour-keyed variant) or -1 */
    int tw, th;                                 /* texture size in texels */
    int sx, sy, sw, sh;                         /* scissor rectangle inside the render target (sw,sh > 0) */
    float tfac[4];                              /* texture factor r,g,b,a (0..1) */
} GpuState;

int  gpu_init(void);                                  /* 1 when Metal is usable (idempotent) */
const char *gpu_error(void);                          /* last init / runtime error text ("" if none) */
int  gpu_failed(void);                                /* 1 after a GPU command buffer failed: stop using the GPU path */

void gpu_rt_set(int w,int h);                         /* make a w*h render target current (contents undefined until uploaded) */
void gpu_rt_upload(const uint32_t *argb,int w,int h); /* ARGB8888 (0xAARRGGBB) pixels, w*h */
void gpu_rt_read(uint32_t *argb);                     /* waits for the GPU, then copies the render target out (ARGB8888) */
void gpu_tex_upload(int slot,const uint32_t *argb,const uint8_t *keymask,int w,int h);   /* keymask: 0/1 per texel or NULL */
void gpu_draw(const GpuState *st,const float *verts,int nverts);
void gpu_sync(void);                                  /* submit everything recorded so far and wait for it */

/* counters since start: syncs, total wait ms, triangles' vertices, state changes */
void gpu_stats(unsigned long *syncs,double *wait_ms,unsigned long *verts,unsigned long *runs);

#ifdef __cplusplus
}
#endif
