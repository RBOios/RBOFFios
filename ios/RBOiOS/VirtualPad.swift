import SwiftUI

/// On-screen analog stick + action buttons + Pause.
/// Look and layout are ported from UnleashedRecomp's touch_controls.cpp (stick, face buttons, START pill).
/// Stick -> arrow keys, A/S/D/F -> 약 공격 / 강 공격 / 방어 / 특수, "1" -> Pause.
struct VirtualPad: View {
    // Stick frame as fractions of the screen (x, y, width, height), same default as the original layout.
    private let stickFrame = CGRect(x: 0.055, y: 0.560, width: 0.190, height: 0.315)
    // Round buttons: centre as a fraction of the screen, diameter as a fraction of the screen height.
    private let buttons: [(label: String, key: Int32, cx: CGFloat, cy: CGFloat)] = [
        ("특수",   DIK.f, 0.7925, 0.5125),   // top
        ("방어",   DIK.d, 0.7325, 0.6425),   // left
        ("강 공격", DIK.s, 0.8525, 0.6425),   // right
        ("약 공격", DIK.a, 0.7925, 0.7725),   // bottom
    ]
    private let buttonDiameter: CGFloat = 0.135
    // Pause pill (the original START button's look), top centre.
    private let pauseFrame = CGRect(x: 0.4575, y: 0.045, width: 0.085, height: 0.112)

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            ZStack {
                AnalogStick()
                    .frame(width: stickFrame.width * w, height: stickFrame.height * h)
                    .position(x: stickFrame.midX * w, y: stickFrame.midY * h)
                ForEach(buttons, id: \.label) { b in
                    PadButton(label: b.label, key: b.key, fontScale: 0.30)
                        .frame(width: buttonDiameter * h, height: buttonDiameter * h)
                        .position(x: b.cx * w, y: b.cy * h)
                }
                PadButton(label: "Pause", key: DIK.one, fontScale: 0.30)
                    .frame(width: pauseFrame.width * w, height: pauseFrame.height * h)
                    .position(x: pauseFrame.midX * w, y: pauseFrame.midY * h)
            }
            .frame(width: w, height: h)
        }
    }
}

/// Floating analog stick: deflection is measured from where the finger first touched.
struct AnalogStick: View {
    private static let deadzone: CGFloat = 0.14          // STICK_DEADZONE
    private static let activationRadius: CGFloat = 0.48  // STICK_ACTIVATION_RADIUS
    @State private var knob: CGSize = .zero

    var body: some View {
        GeometryReader { geo in
            let size = min(geo.size.width, geo.size.height)
            let outer = size * Self.activationRadius
            let knobR = size * 0.14
            ZStack {
                Circle().fill(Color.white.opacity(0.10))
                Circle().stroke(Color.white.opacity(0.38), lineWidth: 1.5)
                ZStack {
                    Circle().fill(Color.white.opacity(0.18))
                    Circle().stroke(Color.white.opacity(0.72), lineWidth: 1.5)
                }
                .frame(width: knobR * 2, height: knobR * 2)
                .offset(knob)
            }
            .frame(width: size, height: size)
            .frame(width: geo.size.width, height: geo.size.height)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { v in
                    var d = CGSize(width: v.location.x - v.startLocation.x,
                                   height: v.location.y - v.startLocation.y)
                    let dist = hypot(d.width, d.height)
                    if dist > outer && dist > 0 { d.width *= outer / dist; d.height *= outer / dist }
                    knob = d
                    let nx = d.width / outer, ny = d.height / outer
                    let mag = hypot(nx, ny)
                    if mag < Self.deadzone || mag <= 0 {
                        PadInput.axis(0, 0)
                    } else {
                        let r = min(max((mag - Self.deadzone) / (1 - Self.deadzone), 0), 1)
                        PadInput.axis(Float(nx / mag * r), Float(-ny / mag * r))   // screen Y is down, pad Y is up
                    }
                }
                .onEnded { _ in
                    knob = .zero
                    PadInput.axis(0, 0)
                })
        }
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
