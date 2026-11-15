import GameController
import Combine

/// Physical gamepads (GCController) -> DirectInput keys.
/// `controllerConnected` is true while at least one usable controller is connected;
/// the on-screen touch pad is hidden for as long as it is true.
///
/// Uses `physicalInputProfile` (iOS 14+) instead of `extendedGamepad`, so controllers WITHOUT analog sticks
/// (8BitDo Micro, which iOS exposes only as a micro/basic gamepad) work just like full controllers (SN30 Pro etc.).
final class PadInput: ObservableObject {
    @Published private(set) var controllerConnected = false

    private var attached = Set<ObjectIdentifier>()
    private var started = false

    /// Merges d-pad + left stick of one controller so the two handlers don't cancel each other out.
    private final class AxisState { var dx: Float = 0, dy: Float = 0, sx: Float = 0, sy: Float = 0 }

    func start() {
        guard !started else { return }
        started = true
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.attach(c) }
            self?.refresh(excluding: nil)
        }
        NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.attached.remove(ObjectIdentifier(c)) }
            self?.refresh(excluding: n.object as? GCController)
            PadInput.releaseAll()   // nothing may stay pressed once the pad is gone
        }
        GCController.controllers().forEach(attach)
        refresh(excluding: nil)     // controllers that were already connected before the game started
        GCController.startWirelessControllerDiscovery(completionHandler: {})
    }

    /// A controller is usable if it has face buttons and some kind of direction input (d-pad or stick).
    private func isUsable(_ c: GCController) -> Bool {
        let p = c.physicalInputProfile
        let hasDir = p.dpads[GCInputDirectionPad] != nil || p.dpads[GCInputLeftThumbstick] != nil
        let hasBtn = p.buttons[GCInputButtonA] != nil || p.buttons[GCInputButtonB] != nil
        return hasDir && hasBtn
    }

    /// Updates `controllerConnected`. `gone` is the controller that is just being disconnected (it may still be in the list).
    private func refresh(excluding gone: GCController?) {
        let connected = GCController.controllers().contains { $0 !== gone && isUsable($0) }
        if connected && !controllerConnected {
            PadInput.releaseAll()   // the touch pad disappears now: drop anything it was still holding
        }
        controllerConnected = connected
    }

    private func attach(_ c: GCController) {
        guard isUsable(c), attached.insert(ObjectIdentifier(c)).inserted else { return }
        let p = c.physicalInputProfile
        let st = AxisState()

        p.dpads[GCInputDirectionPad]?.valueChangedHandler = { _, x, y in
            st.dx = x; st.dy = y; PadInput.axis(st.dx + st.sx, st.dy + st.sy)
        }
        p.dpads[GCInputLeftThumbstick]?.valueChangedHandler = { _, x, y in
            st.sx = x; st.sy = y; PadInput.axis(st.dx + st.sx, st.dy + st.sy)
        }

        func bind(_ name: String, _ key: Int32) {
            p.buttons[name]?.pressedChangedHandler = { _, _, pressed in rbo_set_key(key, pressed ? 1 : 0) }
        }
        bind(GCInputButtonA, DIK.a)
        bind(GCInputButtonB, DIK.s)
        bind(GCInputButtonX, DIK.d)
        bind(GCInputButtonY, DIK.f)
        // The on-screen PAUSE button is hidden while a controller is connected, so the controller's Menu/Start (+) button pauses instead.
        bind(GCInputButtonMenu, DIK.one)
    }

    static func axis(_ x: Float, _ y: Float) {
        rbo_set_key(DIK.left, x < -0.4 ? 1 : 0); rbo_set_key(DIK.right, x > 0.4 ? 1 : 0)
        rbo_set_key(DIK.up, y > 0.4 ? 1 : 0);    rbo_set_key(DIK.down, y < -0.4 ? 1 : 0)
    }

    static func releaseAll() {
        axis(0, 0)
        for key in [DIK.a, DIK.s, DIK.d, DIK.f, DIK.one] { rbo_set_key(key, 0) }
    }
}
