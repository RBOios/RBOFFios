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
    @StateObject private var pad = PadInput()   // physical gamepad; also tells us when to hide the touch pad
    // Performance readout (top-left): frames shown per second and average ms spent converting/handing over each frame.
    private let showOverlay = false   // 화면 왼쪽 위 성능 표시. true로 바꾸면 다시 보임 (rbo_perf.txt 기록은 이 값과 상관없이 계속됨)
    @State private var statsText = ""
    @State private var profText = ""
    @State private var logHandle: FileHandle? = nil
    @State private var logStart = Date()
    @State private var lastFrames: Int32 = 0
    @State private var lastPresentMs: Double = 0
    @State private var lastIdleMs: Double = 0
    @State private var lastInsns: UInt64 = 0
    private let statsTimer = Timer.publish(every: 1.0, on: .main, in: .common).autoconnect()

    private func openLog() {
        guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
        let url = docs.appendingPathComponent("rbo_perf.txt")          // one fixed file, overwritten each launch
        FileManager.default.createFile(atPath: url.path, contents: nil)   // creates or truncates
        guard let h = try? FileHandle(forWritingTo: url) else { return }
        logHandle = h; logStart = Date()
        logLine("RBO perf log · \(UIDevice.current.model) · iOS \(UIDevice.current.systemVersion) · \(edition.exeName)")
    }
    private func logLine(_ s: String) {
        guard let h = logHandle, let d = (s + "\n").data(using: .utf8) else { return }
        try? h.write(contentsOf: d)
    }

    var body: some View {
        ZStack {
            if let host = host { MetalGameView(host: host).ignoresSafeArea() }
            if !pad.controllerConnected { VirtualPad() }   // hidden while a Bluetooth/MFi gamepad is connected
            if showOverlay {
                VStack {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(statsText)
                            Text(profText)
                        }
                        .font(.system(size: 10, weight: .medium, design: .monospaced)).foregroundColor(.green)
                        .padding(4).background(Color.black.opacity(0.5)).cornerRadius(4)
                        Spacer()
                    }
                    Spacer()
                }.padding(8).allowsHitTesting(false)
            }
            if finished {
                VStack(spacing: 12) {
                    Text("게임이 종료되었어요").foregroundColor(.white)
                    Text("에디션을 바꾸려면 앱을 다시 실행해 주세요 (엔진 상태가 전역이라 한 번만 시작할 수 있어요).").font(.footnote).foregroundColor(.gray)
                    Button("앱 종료") { exit(0) }.foregroundColor(.cyan)
                }.padding(24).background(Color.black.opacity(0.85)).cornerRadius(12)
            }
        }
        .onReceive(statsTimer) { _ in
            var f: Int32 = 0; var ms: Double = 0; var idle: Double = 0; var insns: UInt64 = 0
            rbo_get_stats2(&f, &ms, &idle, &insns)
            let df = Int(f - lastFrames), dms = ms - lastPresentMs
            let didle = idle - lastIdleMs, dins = insns &- lastInsns
            lastFrames = f; lastPresentMs = ms; lastIdleMs = idle; lastInsns = insns
            // 대기 = 1초 중 잠들어 있던 비율(%), MIPS = 초당 실행한 x86 명령어(백만 단위)
            var pbuf = [CChar](repeating: 0, count: 4096)
            _ = rbo_get_prof(&pbuf, Int32(pbuf.count))
            let fullProf = String(cString: pbuf)
            profText = fullProf.split(separator: "\n", omittingEmptySubsequences: false).prefix(7).joined(separator: "\n")
            // one log line per second: Files 앱 > 내 iPhone > RBO 폴더의 rbo_perf.txt (실행할 때마다 덮어씀)
            let thermal = ProcessInfo.processInfo.thermalState.rawValue      // 0 정상, 1 약간 뜨거움, 2 심함, 3 위험
            let lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled ? 1 : 0
            let head = String(format: "t=%d frames=%d fps=%d conv=%.1fms mips=%.0f thermal=%d lowpower=%d | ",
                              Int32(Date().timeIntervalSince(logStart)), f, df, df > 0 ? dms / Double(df) : 0,
                              Double(dins) / 1_000_000.0, thermal, lowPower)
            logLine(head + fullProf.replacingOccurrences(of: "\n", with: " | "))
            statsText = String(format: "%d FPS · 화면변환 %.1fms · 대기 %.0f%% · %.0f MIPS", df, df > 0 ? dms / Double(df) : 0, didle / 10.0, Double(dins) / 1_000_000.0)
        }
        .onAppear {
            guard let p = MetalPresenter.make() else { return }
            let h = EmulatorHost(presenter: p)
            h.onFinished = { finished = true; logLine("게임 종료") }
            openLog()
            host = h; pad.start()
            h.start(root: root, exe: edition.exeName)
        }
    }
}
