import MetalKit

/// Uploads the emulator's BGRA frame to a texture and draws it aspect-fit.
final class MetalPresenter: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let sampler: MTLSamplerState
    private var texture: MTLTexture?
    private var pending = [UInt32]()
    private var pw = 0, ph = 0
    private var dirty = false
    private let lock = NSLock()

    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct VOut { float4 pos [[position]]; float2 uv; };
    vertex VOut vs(uint vid [[vertex_id]], constant float2 &scale [[buffer(0)]]) {
        float2 p[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };
        float2 t[4] = { float2(0,1), float2(1,1), float2(0,0), float2(1,0) };
        VOut o; o.pos = float4(p[vid] * scale, 0, 1); o.uv = t[vid]; return o;
    }
    fragment float4 fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
        return tex.sample(s, in.uv);
    }
    """

    static func make() -> MetalPresenter? { MetalPresenter(metal: ()) }

    /// Call with the MTKView that is actually shown on screen.
    func attach(to view: MTKView) {
        view.device = device
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.preferredFramesPerSecond = 60
        view.delegate = self
    }

    private init?(metal: Void) {
        guard let dev = MTLCreateSystemDefaultDevice(), let q = dev.makeCommandQueue() else { return nil }
        device = dev; queue = q
        guard let lib = try? dev.makeLibrary(source: MetalPresenter.shaderSource, options: nil) else { return nil }
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = lib.makeFunction(name: "vs"); d.fragmentFunction = lib.makeFunction(name: "fs")
        d.colorAttachments[0].pixelFormat = .bgra8Unorm
        guard let p = try? dev.makeRenderPipelineState(descriptor: d) else { return nil }
        pipeline = p
        let sd = MTLSamplerDescriptor(); sd.minFilter = .linear; sd.magFilter = .linear
        sampler = dev.makeSamplerState(descriptor: sd)!
        super.init()
    }

    /// Emulator thread.
    func submit(_ px: UnsafePointer<UInt32>, w: Int, h: Int) {
        lock.lock(); defer { lock.unlock() }
        if pending.count != w * h { pending = [UInt32](repeating: 0, count: w * h) }
        pending.withUnsafeMutableBytes { _ = memcpy($0.baseAddress!, px, w * h * 4) }
        pw = w; ph = h; dirty = true
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        lock.lock()
        if dirty && pw > 0 {
            if texture == nil || texture!.width != pw || texture!.height != ph {
                let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: pw, height: ph, mipmapped: false)
                texture = device.makeTexture(descriptor: td)
            }
            pending.withUnsafeBytes { texture!.replace(region: MTLRegionMake2D(0, 0, pw, ph), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: pw * 4) }
            dirty = false
        }
        let w = pw, h = ph
        lock.unlock()
        guard let tex = texture, let drawable = view.currentDrawable, let rpd = view.currentRenderPassDescriptor,
              let cb = queue.makeCommandBuffer(), let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }
        let va = Float(view.drawableSize.width / max(view.drawableSize.height, 1)), ta = Float(w) / Float(max(h, 1))
        var scale = va > ta ? SIMD2<Float>(ta / va, 1) : SIMD2<Float>(1, va / ta)
        enc.setRenderPipelineState(pipeline)
        enc.setVertexBytes(&scale, length: MemoryLayout<SIMD2<Float>>.size, index: 0)
        enc.setFragmentTexture(tex, index: 0); enc.setFragmentSamplerState(sampler, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding(); cb.present(drawable); cb.commit()
    }
}
