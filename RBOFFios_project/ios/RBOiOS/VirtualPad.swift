import SwiftUI

/// On-screen D-pad + action buttons + PAUSE button.
/// D-pad -> arrow keys (8 directions), A/S/D/F -> 약 공격 / 강 공격 / 방어 / 특수, PAUSE image -> "1" (Pause).
struct VirtualPad: View {
    // D-pad frame as fractions of the screen (x, y, width, height); the pad is drawn as the largest square that fits.
    private let dpadFrame = CGRect(x: 0.055, y: 0.560, width: 0.190, height: 0.315)
    // Game picture is drawn aspect-fit and centred (Expansion 3 renders 1280x1024); the rest of the screen is black bars.
    private let gameAspect: CGFloat = 1280.0 / 1024.0
    // Round buttons, laid out in a diamond. Units: fractions of the screen height, relative to the cluster centre.
    private let buttons: [(label: String, key: Int32, dx: CGFloat, dy: CGFloat)] = [
        ("특수",   DIK.f,  0.0,    -0.130),   // top
        ("방어",   DIK.d, -0.1075,  0.0),     // left
        ("강 공격", DIK.s,  0.1075,  0.0),     // right
        ("약 공격", DIK.a,  0.0,     0.130),   // bottom
    ]
    private let buttonDiameter: CGFloat = 0.135   // of screen height
    private let clusterCentreY: CGFloat = 0.70    // of screen height
    private let pauseWidth: CGFloat = 0.20        // PAUSE button width, of screen height (the image keeps its own aspect ratio)
    private let pauseTopMargin: CGFloat = 0.05    // gap between PAUSE and the top edge, of screen height

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            // Centre of the right-hand black bar, so the buttons stay off the game picture
            // (on screens with no bar, e.g. iPad, they just stay inside the right edge).
            let barWidth = max((w - gameAspect * h) / 2, 0)
            let clusterCX = min(w - barWidth / 2, w - 0.205 * h)
            ZStack {
                DPad()
                    .frame(width: dpadFrame.width * w, height: dpadFrame.height * h)
                    .position(x: dpadFrame.midX * w, y: dpadFrame.midY * h)
                ForEach(buttons, id: \.label) { b in
                    PadButton(label: b.label, key: b.key, fontScale: 0.30)
                        .frame(width: buttonDiameter * h, height: buttonDiameter * h)
                        .position(x: clusterCX + b.dx * h, y: clusterCentreY * h + b.dy * h)
                }
                PauseButton(key: DIK.one)
                    .frame(width: pauseWidth * h)
                    .position(x: clusterCX, y: pauseTopMargin * h + ((pauseWidth * h - 20) * 0.392 + 20) / 2)   // top right, above the button cluster
            }
            .frame(width: w, height: h)
        }
        .ignoresSafeArea()
    }
}

/// Four-piece D-pad image (outline only, transparent background). Touch position -> up/down/left/right, diagonals when two sectors overlap.
struct DPad: View {
    private struct Dir: Equatable { var l = false, r = false, u = false, d = false }
    private static let idleOpacity: Double = 0.80     // pieces at rest
    private static let deadzone: CGFloat = 0.12       // fraction of the pad radius where nothing is pressed
    private static let sector: CGFloat = 0.38         // sin(22.5 deg): 8-way split
    @State private var dir = Dir()

    var body: some View {
        GeometryReader { geo in
            let size = min(geo.size.width, geo.size.height)
            ZStack {
                piece("DPadUp", dir.u)
                piece("DPadDown", dir.d)
                piece("DPadLeft", dir.l)
                piece("DPadRight", dir.r)
            }
            .frame(width: size, height: size)
            .frame(width: geo.size.width, height: geo.size.height)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { v in
                    update(dx: v.location.x - geo.size.width / 2, dy: v.location.y - geo.size.height / 2, radius: size / 2)
                }
                .onEnded { _ in set(Dir()) })
        }
    }

    private func piece(_ name: String, _ pressed: Bool) -> some View {
        Image(name)
            .resizable()
            .scaledToFit()
            .opacity(pressed ? 1.0 : Self.idleOpacity)
            .brightness(pressed ? 0.30 : 0)
    }

    private func update(dx: CGFloat, dy: CGFloat, radius: CGFloat) {
        let dist = hypot(dx, dy)
        var nd = Dir()
        if dist > radius * Self.deadzone && dist > 0 {
            let nx = dx / dist, ny = dy / dist   // screen Y is down
            nd.l = nx < -Self.sector; nd.r = nx > Self.sector
            nd.u = ny < -Self.sector; nd.d = ny > Self.sector
        }
        set(nd)
    }

    private func set(_ nd: Dir) {
        guard nd != dir else { return }
        dir = nd
        PadInput.axis(nd.l ? -1 : (nd.r ? 1 : 0), nd.u ? 1 : (nd.d ? -1 : 0))   // pad Y is up
    }
}

/// PAUSE image button (transparent background, outline only). Sends the Pause key. A short tap is held for at least 0.1 s so the game never misses it.
struct PauseButton: View {
    let key: Int32
    @State private var held = false
    @State private var pressedAt = Date()
    @State private var token = 0

    var body: some View {
        Image("PauseButton")
            .resizable()
            .scaledToFit()
            .opacity(held ? 1.0 : 0.80)
            .brightness(held ? 0.30 : 0)
            .padding(10)                       // bigger touch target than the picture itself
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !held else { return }
                    held = true; pressedAt = Date(); token += 1
                    rbo_set_key(key, 1)
                }
                .onEnded { _ in
                    held = false
                    let id = token
                    let wait = max(0, 0.10 - Date().timeIntervalSince(pressedAt))
                    DispatchQueue.main.asyncAfter(deadline: .now() + wait) {
                        if token == id { rbo_set_key(key, 0) }   // ignore if pressed again meanwhile
                    }
                })
    }
}

/// Round button with a text label. A short tap is held for at least 0.1 s so the game never misses it.
struct PadButton: View {
    let label: String
    let key: Int32
    var fontScale: CGFloat = 0.30
    @State private var held = false
    @State private var pressedAt = Date()
    @State private var token = 0

    var body: some View {
        GeometryReader { geo in
            let m = min(geo.size.width, geo.size.height)
            ZStack {
                Capsule().fill(Color.white.opacity(held ? 0.35 : 0.08))
                Capsule().stroke(Color.white.opacity(held ? 0.80 : 0.32), lineWidth: 1.5)
                Text(label)
                    .font(.system(size: m * fontScale, weight: .bold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.5)
                    .padding(.horizontal, m * 0.08)
                    .foregroundColor(Color.white.opacity(held ? 1.0 : 0.85))
            }
            .contentShape(Capsule())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !held else { return }
                    held = true; pressedAt = Date(); token += 1
                    rbo_set_key(key, 1)
                }
                .onEnded { _ in
                    held = false
                    let id = token
                    let wait = max(0, 0.10 - Date().timeIntervalSince(pressedAt))
                    DispatchQueue.main.asyncAfter(deadline: .now() + wait) {
                        if token == id { rbo_set_key(key, 0) }   // ignore if pressed again meanwhile
                    }
                })
        }
    }
}
