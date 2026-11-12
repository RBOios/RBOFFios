import GameController
import SwiftUI

/// Physical gamepads (GCController) -> DirectInput keys.
final class PadInput {
    func start() {
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.attach(c) }
        }
        GCController.controllers().forEach(attach)
        GCController.startWirelessControllerDiscovery(completionHandler: {})
    }
    private func attach(_ c: GCController) {
        guard let g = c.extendedGamepad else { return }
        g.dpad.valueChangedHandler = { _, x, y in PadInput.axis(x, y) }
        g.leftThumbstick.valueChangedHandler = { _, x, y in PadInput.axis(x, y) }
        g.buttonA.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.a, p ? 1 : 0) }
        g.buttonB.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.s, p ? 1 : 0) }
        g.buttonX.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.d, p ? 1 : 0) }
        g.buttonY.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.f, p ? 1 : 0) }
    }
    private static func axis(_ x: Float, _ y: Float) {
        rbo_set_key(DIK.left, x < -0.4 ? 1 : 0); rbo_set_key(DIK.right, x > 0.4 ? 1 : 0)
        rbo_set_key(DIK.up, y > 0.4 ? 1 : 0);    rbo_set_key(DIK.down, y < -0.4 ? 1 : 0)
    }
}

/// On-screen controls (touch). Each key is held while the finger is down.
struct VirtualPad: View {
    var body: some View {
        VStack {
            Spacer()
            HStack(alignment: .bottom) {
                dpad.padding(.leading, 24)
                Spacer()
                actions.padding(.trailing, 24)
            }.padding(.bottom, 16)
        }
    }
    private var dpad: some View {
        VStack(spacing: 6) {
            PadKey("↑", DIK.up)
            HStack(spacing: 6) { PadKey("←", DIK.left); Color.clear.frame(width: 56, height: 56); PadKey("→", DIK.right) }
            PadKey("↓", DIK.down)
        }
    }
    private var actions: some View {
        HStack(spacing: 8) { PadKey("A", DIK.a); PadKey("S", DIK.s); PadKey("D", DIK.d); PadKey("F", DIK.f) }
    }
}

struct PadKey: View {
    let label: String; let key: Int32
    @State private var down = false
    init(_ l: String, _ k: Int32) { label = l; key = k }
    var body: some View {
        Text(label).font(.system(size: 22, weight: .bold)).foregroundColor(.white)
            .frame(width: 56, height: 56)
            .background(Circle().fill(Color.white.opacity(down ? 0.45 : 0.18)))
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in if !down { down = true; rbo_set_key(key, 1) } }
                .onEnded { _ in down = false; rbo_set_key(key, 0) })
    }
}
