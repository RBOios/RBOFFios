/* gpu_raster.m - Metal back end for d3d.c (see gpu_raster.h).  Plain Objective-C, ARC.
 * The shader is compiled from source at run time (like MetalPresenter), so a problem here can never break the build:
 * gpu_init() just returns 0 and d3d.c keeps using the CPU rasterizer. */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "gpu_raster.h"

#define MAXTEX      1024                /* 2 x MAXS in dx.c: one slot per DirectDraw surface + one for its colour-keyed variant */
#define VB_VERTS    (96*1024)           /* vertex ring: 96K vertices * 32 bytes = 3 MB, reset after every sync */
#define VERT_BYTES  32

/* ---- uniforms: must match struct U in the shader (20 scalars, 80 bytes) ---- */
typedef struct {
    float rtw, rth, tw, th;
    int tex_on, cop, aop, ca1, ca2, aa1, aa2, use_key, atest, afunc, aref, point;
    float tf0, tf1, tf2, tf3;
} GpuU;

#define MSL(...) #__VA_ARGS__
static const char *kShaderSrc =
"#include <metal_stdlib>\nusing namespace metal;\n"
MSL(
struct GV { float4 pu; float4 c; };
struct U {
    float rtw; float rth; float tw; float th;
    int tex_on; int cop; int aop; int ca1; int ca2; int aa1; int aa2; int use_key; int atest; int afunc; int aref; int point;
    float tf0; float tf1; float tf2; float tf3;
};
struct VOut { float4 pos [[position]]; float4 col; float2 uv; };

vertex VOut rbo_vs(uint vid [[vertex_id]], constant U &u [[buffer(0)]], const device GV *vb [[buffer(1)]])
{
    GV g = vb[vid];
    VOut o;
    o.pos = float4(g.pu.x / u.rtw * 2.0 - 1.0, 1.0 - g.pu.y / u.rth * 2.0, 0.0, 1.0);
    o.col = g.c;
    o.uv = g.pu.zw;
    return o;
}

float3 rbo_sel3(int a, float3 d, float3 t, float3 f) { if (a == 2) return t; if (a == 3) return f; return d; }
float  rbo_sel1(int a, float d, float t, float f)    { if (a == 2) return t; if (a == 3) return f; return d; }

fragment float4 rbo_fs(VOut in [[stage_in]], constant U &u [[buffer(0)]],
                       texture2d<float> tex [[texture(0)]], texture2d<float> keytex [[texture(1)]],
                       sampler smp [[sampler(0)]])
{
    float4 diff = in.col;
    float3 cc = diff.rgb;
    float aa = diff.a;
    if (u.tex_on != 0) {
        float2 uv = in.uv;
        if (u.point != 0) {
            float2 ts = float2(u.tw, u.th);
            uv = (floor(uv * ts + 0.004) + 0.5) / ts;
        }
        float4 tx = tex.sample(smp, uv);
        if (u.use_key != 0) {
            float k = keytex.sample(smp, uv).r;
            if (k > 0.999) { discard_fragment(); }
        }
        float3 tf3v = float3(u.tf0, u.tf1, u.tf2);
        float3 c1 = rbo_sel3(u.ca1, diff.rgb, tx.rgb, tf3v);
        float3 c2 = rbo_sel3(u.ca2, diff.rgb, tx.rgb, tf3v);
        switch (u.cop) {
            case 2:  cc = c1; break;
            case 3:  cc = c2; break;
            case 5:  cc = min(c1 * c2 * 2.0, float3(1.0)); break;
            case 6:  cc = min(c1 * c2 * 4.0, float3(1.0)); break;
            case 7:  cc = min(c1 + c2, float3(1.0)); break;
            default: cc = c1 * c2; break;
        }
        float a1 = rbo_sel1(u.aa1, diff.a, tx.a, u.tf3);
        float a2 = rbo_sel1(u.aa2, diff.a, tx.a, u.tf3);
        switch (u.aop) {
            case 1:  aa = diff.a; break;
            case 2:  aa = a1; break;
            case 3:  aa = a2; break;
            case 5:  aa = min(a1 * a2 * 2.0, 1.0); break;
            case 7:  aa = min(a1 + a2, 1.0); break;
            default: aa = a1 * a2; break;
        }
    }
    if (u.atest != 0) {
        int a8 = int(aa * 255.0 + 0.5);
        bool ok = true;
        switch (u.afunc) {
            case 1:  ok = false; break;
            case 2:  ok = a8 < u.aref; break;
            case 3:  ok = a8 == u.aref; break;
            case 4:  ok = a8 <= u.aref; break;
            case 5:  ok = a8 > u.aref; break;
            case 6:  ok = a8 != u.aref; break;
            case 7:  ok = a8 >= u.aref; break;
            default: ok = true; break;
        }
        if (!ok) { discard_fragment(); }
    }
    return float4(cc, aa);
}
);

/* ---- state ---- */
static id<MTLDevice>              g_dev;
static id<MTLCommandQueue>        g_queue;
static id<MTLFunction>            g_vfn, g_ffn;
static id<MTLBuffer>              g_vb;
static id<MTLTexture>             g_rt;
static int                        g_rtw, g_rth;
static id<MTLTexture>             g_tex[MAXTEX], g_key[MAXTEX];
static unsigned                   g_tex_epoch[MAXTEX];     /* epoch in which the slot was last bound (== g_epoch: GPU work in flight uses it) */
static id<MTLTexture>             g_dummy_tex, g_dummy_key;
static id<MTLSamplerState>        g_smp[6];
static NSMutableDictionary       *g_pipes;
static id<MTLCommandBuffer>       g_cb;
static id<MTLRenderCommandEncoder> g_enc;
static unsigned                   g_epoch = 1;
static int                        g_inited = 0, g_ok = 0, g_dead = 0;
static char                       g_err[256];

static int      g_vtx_used = 0;
static int      g_run_start = 0, g_run_n = 0;
static GpuState g_state; static int g_have_state = 0;

static unsigned long g_n_sync = 0, g_n_verts = 0, g_n_runs = 0; static double g_wait_ms = 0;

static void set_err(const char *what, NSError *e) {
    const char *d = e ? [[e localizedDescription] UTF8String] : "";
    snprintf(g_err, sizeof g_err, "%s: %s", what, d ? d : "");
    NSLog(@"[GPU] %s", g_err);
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

const char *gpu_error(void) { return g_err; }
int gpu_failed(void) { return g_dead; }
void gpu_stats(unsigned long *syncs, double *wait_ms, unsigned long *verts, unsigned long *runs) {
    if (syncs) *syncs = g_n_sync; if (wait_ms) *wait_ms = g_wait_ms; if (verts) *verts = g_n_verts; if (runs) *runs = g_n_runs;
}

static MTLBlendFactor bfac(int f) {
    switch (f) {
        case 1:  return MTLBlendFactorZero;
        case 2:  return MTLBlendFactorOne;
        case 3:  return MTLBlendFactorSourceColor;
        case 4:  return MTLBlendFactorOneMinusSourceColor;
        case 5:  return MTLBlendFactorSourceAlpha;
        case 6:  return MTLBlendFactorOneMinusSourceAlpha;
        case 7:  return MTLBlendFactorDestinationAlpha;
        case 8:  return MTLBlendFactorOneMinusDestinationAlpha;
        case 9:  return MTLBlendFactorDestinationColor;
        case 10: return MTLBlendFactorOneMinusDestinationColor;
        default: return MTLBlendFactorOne;      /* the CPU rasterizer treats every other factor as 1 */
    }
}

static id<MTLRenderPipelineState> pipeline_for(const GpuState *st) {
    int blend = st->blend ? 1 : 0, sf = blend ? st->sf : 0, df = blend ? st->df : 0, wa = st->write_alpha ? 1 : 0;
    NSNumber *key = @(blend | (sf << 1) | (df << 6) | (wa << 11));
    id<MTLRenderPipelineState> p = g_pipes[key];
    if (p) return p;
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = g_vfn; d.fragmentFunction = g_ffn;
    MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.writeMask = wa ? MTLColorWriteMaskAll : (MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue);
    if (blend) {
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd; ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = bfac(sf); ca.destinationRGBBlendFactor = bfac(df);
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne; ca.destinationAlphaBlendFactor = MTLBlendFactorZero;   /* result alpha = source alpha, like the CPU path */
    }
    NSError *e = nil;
    p = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) { set_err("pipeline", e); g_dead = 1; return nil; }
    g_pipes[key] = p;
    return p;
}

static id<MTLTexture> make_tex(MTLPixelFormat fmt, int w, int h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    return [g_dev newTextureWithDescriptor:td];
}

int gpu_init(void) {
    if (g_inited) return g_ok;
    g_inited = 1; g_ok = 0; g_err[0] = 0;
    @autoreleasepool {
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) { snprintf(g_err, sizeof g_err, "no Metal device"); return 0; }
        g_queue = [g_dev newCommandQueue];
        if (!g_queue) { snprintf(g_err, sizeof g_err, "no command queue"); return 0; }
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:kShaderSrc] options:nil error:&e];
        if (!lib) { set_err("shader compile", e); return 0; }
        g_vfn = [lib newFunctionWithName:@"rbo_vs"]; g_ffn = [lib newFunctionWithName:@"rbo_fs"];
        if (!g_vfn || !g_ffn) { snprintf(g_err, sizeof g_err, "shader functions missing"); return 0; }
        g_vb = [g_dev newBufferWithLength:(NSUInteger)VB_VERTS * VERT_BYTES options:MTLResourceStorageModeShared];
        if (!g_vb) { snprintf(g_err, sizeof g_err, "vertex buffer"); return 0; }
        g_pipes = [[NSMutableDictionary alloc] init];
        g_dummy_tex = make_tex(MTLPixelFormatBGRA8Unorm, 1, 1); g_dummy_key = make_tex(MTLPixelFormatR8Unorm, 1, 1);
        if (!g_dummy_tex || !g_dummy_key) { snprintf(g_err, sizeof g_err, "dummy textures"); return 0; }
        { uint32_t w = 0xFFFFFFFFu; uint8_t z = 0;
          [g_dummy_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&w bytesPerRow:4];
          [g_dummy_key replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&z bytesPerRow:1]; }
        for (int lin = 0; lin < 2; lin++) for (int ad = 0; ad < 3; ad++) {
            MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
            sd.minFilter = sd.magFilter = lin ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            MTLSamplerAddressMode am = ad == 0 ? MTLSamplerAddressModeRepeat : (ad == 1 ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeClampToEdge);
            sd.sAddressMode = am; sd.tAddressMode = am;
            g_smp[lin * 3 + ad] = [g_dev newSamplerStateWithDescriptor:sd];
            if (!g_smp[lin * 3 + ad]) { snprintf(g_err, sizeof g_err, "sampler"); return 0; }
        }
        GpuState st; memset(&st, 0, sizeof st);                 /* warm the two commonest pipelines so the first frame does not hitch */
        st.write_alpha = 0; pipeline_for(&st);
        st.blend = 1; st.sf = 5; st.df = 6; pipeline_for(&st);
        if (g_dead) return 0;
    }
    g_ok = 1;
    NSLog(@"[GPU] Metal rasterizer ready on %@", [g_dev name]);
    return 1;
}

/* ---- command recording ---- */
static void flush_run(void) {
    if (g_run_n > 0 && g_enc) {
        [g_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:(NSUInteger)g_run_start vertexCount:(NSUInteger)g_run_n];
        g_n_runs++;
    }
    g_run_n = 0;
}

void gpu_sync(void) {
    if (!g_enc && !g_cb) { g_vtx_used = 0; g_have_state = 0; return; }
    @autoreleasepool {
        flush_run();
        double t0 = now_ms();
        if (g_enc) { [g_enc endEncoding]; g_enc = nil; }
        if (g_cb) {
            [g_cb commit]; [g_cb waitUntilCompleted];
            if (g_cb.status == MTLCommandBufferStatusError) { set_err("command buffer", g_cb.error); g_dead = 1; }
            g_cb = nil;
        }
        g_wait_ms += now_ms() - t0; g_n_sync++;
    }
    g_vtx_used = 0; g_have_state = 0; g_epoch++;
}

static void begin_pass(void) {
    if (g_enc) return;
    g_cb = [g_queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g_rt;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    g_enc = [g_cb renderCommandEncoderWithDescriptor:rp];
    [g_enc setVertexBuffer:g_vb offset:0 atIndex:1];
    g_have_state = 0;
}

static void apply_state(const GpuState *st) {
    id<MTLRenderPipelineState> p = pipeline_for(st);
    if (!p) return;
    GpuU u; memset(&u, 0, sizeof u);
    u.rtw = (float)g_rtw; u.rth = (float)g_rth; u.tw = (float)(st->tw > 0 ? st->tw : 1); u.th = (float)(st->th > 0 ? st->th : 1);
    u.tex_on = st->tex_on; u.cop = st->cop; u.aop = st->aop; u.ca1 = st->ca1; u.ca2 = st->ca2; u.aa1 = st->aa1; u.aa2 = st->aa2;
    u.use_key = st->use_key; u.atest = st->atest; u.afunc = st->afunc; u.aref = st->aref; u.point = st->point;
    u.tf0 = st->tfac[0]; u.tf1 = st->tfac[1]; u.tf2 = st->tfac[2]; u.tf3 = st->tfac[3];
    [g_enc setRenderPipelineState:p];
    MTLScissorRect sr; sr.x = (NSUInteger)st->sx; sr.y = (NSUInteger)st->sy; sr.width = (NSUInteger)st->sw; sr.height = (NSUInteger)st->sh;
    [g_enc setScissorRect:sr];
    [g_enc setVertexBytes:&u length:sizeof u atIndex:0];
    [g_enc setFragmentBytes:&u length:sizeof u atIndex:0];
    id<MTLTexture> t = g_dummy_tex, k = g_dummy_key;
    if (st->tex_on && st->tex_id >= 0 && st->tex_id < MAXTEX && g_tex[st->tex_id]) {
        t = g_tex[st->tex_id]; if (g_key[st->tex_id]) k = g_key[st->tex_id];
        g_tex_epoch[st->tex_id] = g_epoch;
    }
    [g_enc setFragmentTexture:t atIndex:0];
    [g_enc setFragmentTexture:k atIndex:1];
    int lin = st->point ? 0 : 1, ad = (st->addr >= 0 && st->addr <= 2) ? st->addr : 0;
    [g_enc setFragmentSamplerState:g_smp[lin * 3 + ad] atIndex:0];
}

void gpu_draw(const GpuState *st, const float *verts, int nverts) {
    if (!g_ok || g_dead || !g_rt || nverts <= 0 || st->sw <= 0 || st->sh <= 0) return;
    int off = 0;
    while (off < nverts) {
        int n = nverts - off; n -= n % 3; if (n <= 0) break;
        if (n > VB_VERTS) n = VB_VERTS - (VB_VERTS % 3);
        if (g_vtx_used + n > VB_VERTS) gpu_sync();                  /* ring full: submit what we have (the render target stays on the GPU) */
        @autoreleasepool {
            begin_pass();
            if (!g_have_state || memcmp(&g_state, st, sizeof *st) != 0) {
                flush_run();
                apply_state(st);
                g_state = *st; g_have_state = 1;
            }
            if (g_run_n == 0) g_run_start = g_vtx_used;
            memcpy((char *)[g_vb contents] + (size_t)g_vtx_used * VERT_BYTES, verts + (size_t)off * 8, (size_t)n * VERT_BYTES);
            g_run_n += n; g_vtx_used += n; g_n_verts += (unsigned long)n;
        }
        off += n;
    }
}

/* ---- render target / textures ---- */
void gpu_rt_set(int w, int h) {
    if (!g_ok || w <= 0 || h <= 0) return;
    if (g_rt && g_rtw == w && g_rth == h) return;
    gpu_sync();
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    g_rt = [g_dev newTextureWithDescriptor:td];
    g_rtw = w; g_rth = h;
    if (!g_rt) { snprintf(g_err, sizeof g_err, "render target texture"); g_dead = 1; }
}

void gpu_rt_upload(const uint32_t *argb, int w, int h) {
    if (!g_ok || !g_rt || w != g_rtw || h != g_rth) return;
    gpu_sync();                                                     /* never overwrite a texture the GPU may still be drawing into */
    [g_rt replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:argb bytesPerRow:(NSUInteger)w * 4];
}

void gpu_rt_read(uint32_t *argb) {
    if (!g_ok || !g_rt) return;
    gpu_sync();
    [g_rt getBytes:argb bytesPerRow:(NSUInteger)g_rtw * 4 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)g_rtw, (NSUInteger)g_rth) mipmapLevel:0];
}

void gpu_tex_upload(int id, const uint32_t *argb, const uint8_t *keymask, int w, int h) {
    if (!g_ok || id < 0 || id >= MAXTEX || w <= 0 || h <= 0) return;
    @autoreleasepool {
        BOOL busy = (g_tex_epoch[id] == g_epoch);                   /* bound by work that has not been submitted yet: must not be overwritten in place */
        id<MTLTexture> t = g_tex[id];
        if (!t || (int)t.width != w || (int)t.height != h || busy) { t = make_tex(MTLPixelFormatBGRA8Unorm, w, h); g_tex[id] = t; g_tex_epoch[id] = 0; }
        if (!t) return;
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:argb bytesPerRow:(NSUInteger)w * 4];
        if (keymask) {
            NSMutableData *md = [NSMutableData dataWithLength:(NSUInteger)w * (NSUInteger)h];
            uint8_t *m = (uint8_t *)[md mutableBytes];
            for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) m[i] = keymask[i] ? 255 : 0;
            id<MTLTexture> k = g_key[id];
            if (!k || (int)k.width != w || (int)k.height != h || busy) { k = make_tex(MTLPixelFormatR8Unorm, w, h); g_key[id] = k; }
            if (k) [k replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:m bytesPerRow:(NSUInteger)w];
        } else g_key[id] = nil;
    }
}
