import SwiftUI
import MetalKit
import GameController

/// DirectInput scan codes used by the game's default P1 key config.
enum DIK {
    static let up: Int32 = 0xC8, down: Int32 = 0xD0, left: Int32 = 0xCB, right: Int32 = 0xCD
    static let a: Int32 = 0x1E, s: Int32 = 0x1F, d: Int32 = 0x20, f: Int32 = 0x21
    static let one: Int32 = 0x02   // Pause
}

final class EmulatorHost {
    let presenter: MetalPresenter
    let audio = AudioOut()
    var onFinished: (() -> Void)?
    init(presenter: MetalPresenter) { self.presenter = presenter }

    func start(root: URL, exe: String) {
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        var cb = RboCallbacks(
            ctx: ctx,
            present: { c, px, w, h in
                let me = Unmanaged<EmulatorHost>.fromOpaque(c!).takeUnretainedValue()
                me.presenter.submit(px!, w: Int(w), h: Int(h))
            },
            finished: { c, _ in
                let me = Unmanaged<EmulatorHost>.fromOpaque(c!).takeUnretainedValue()
                DispatchQueue.main.async { me.audio.stop(); me.onFinished?() }
            })
        audio.start()
        _ = rbo_start(root.path, exe, &cb)
    }
}

struct MetalGameView: UIViewRepresentable {
    let host: EmulatorHost
    func makeUIView(context: Context) -> MTKView {
        let v = MTKView(); host.presenter.attach(to: v); return v
    }
    func updateUIView(_ uiView: MTKView, context: Context) {}
}

struct GameScreen: View {
    let edition: Edition
    let root: URL
    @State private var host: EmulatorHost? = nil
    @State private var finished = false
    @State private var pad = PadInput()
    // Performance readout (top-left): frames shown per second and average ms spent converting/handing over each frame.
    @State private var statsText = ""
    @State private var lastFrames: Int32 = 0
    @State private var lastPresentMs: Double = 0
    private let statsTimer = Timer.publish(every: 1.0, on: .main, in: .common).autoconnect()

    var body: some View {
        ZStack {
            if let host = host { MetalGameView(host: host).ignoresSafeArea() }
            VirtualPad()
            VStack {
                HStack {
                    Text(statsText).font(.system(size: 11, weight: .medium, design: .monospaced)).foregroundColor(.green)
                        .padding(4).background(Color.black.opacity(0.5)).cornerRadius(4)
                    Spacer()
                }
                Spacer()
            }.padding(8).allowsHitTesting(false)
            if finished {
                VStack(spacing: 12) {
                    Text("게임이 종료되었어요").foregroundColor(.white)
                    Text("에디션을 바꾸려면 앱을 다시 실행해 주세요 (엔진 상태가 전역이라 한 번만 시작할 수 있어요).").font(.footnote).foregroundColor(.gray)
                    Button("앱 종료") { exit(0) }.foregroundColor(.cyan)
                }.padding(24).background(Color.black.opacity(0.85)).cornerRadius(12)
            }
        }
        .onReceive(statsTimer) { _ in
            var f: Int32 = 0; var ms: Double = 0
            rbo_get_stats(&f, &ms)
            let df = Int(f - lastFrames), dms = ms - lastPresentMs
            lastFrames = f; lastPresentMs = ms
            statsText = String(format: "%d FPS · 화면변환 %.1fms", df, df > 0 ? dms / Double(df) : 0)
        }
        .onAppear {
            guard let p = MetalPresenter.make() else { return }
            let h = EmulatorHost(presenter: p)
            h.onFinished = { finished = true }
            host = h; pad.start()
            h.start(root: root, exe: edition.exeName)
        }
    }
}
