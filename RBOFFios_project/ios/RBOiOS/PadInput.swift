import GameController
import Combine

/// Physical gamepads (GCController) -> DirectInput keys, following the bindings of GameControls ("컨트롤 설정").
/// `controllerConnected` is true while at least one usable (extended-gamepad) controller is connected;
/// the on-screen touch pad is hidden for as long as it is true.
final class PadInput: ObservableObject {
    @Published private(set) var controllerConnected = false

    func start() {
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.attach(c) }
            self?.refresh(excluding: nil)
        }
        NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] n in
            self?.refresh(excluding: n.object as? GCController)
            PadInput.releaseAll()   // nothing may stay pressed once the pad is gone
        }
        GCController.controllers().forEach(attach)
        refresh(excluding: nil)     // controllers that were already connected before the game started
        GCController.startWirelessControllerDiscovery(completionHandler: {})
    }

    /// Updates `controllerConnected`. `gone` is the controller that is just being disconnected (it may still be in the list).
    private func refresh(excluding gone: GCController?) {
        let connected = GCController.controllers().contains { $0 !== gone && $0.extendedGamepad != nil }
        if connected && !controllerConnected {
            PadInput.releaseAll()   // the touch pad disappears now: drop anything it was still holding
        }
        controllerConnected = connected
    }

    private func attach(_ c: GCController) {
        guard let g = c.extendedGamepad else { return }
        // Every physical control is a ControllerButton; the key it sends is whatever action the player assigned it to.
        func bind(_ input: GCControllerButtonInput?, _ b: ControllerButton) {
            input?.pressedChangedHandler = { _, _, p in PadInput.set("pad.\(b.rawValue)", GameControls.shared.controllerScan(b), p) }
        }
        bind(g.dpad.up, .dpadUp); bind(g.dpad.down, .dpadDown); bind(g.dpad.left, .dpadLeft); bind(g.dpad.right, .dpadRight)
        bind(g.buttonA, .a); bind(g.buttonB, .b); bind(g.buttonX, .x); bind(g.buttonY, .y)
        bind(g.leftShoulder, .l1); bind(g.rightShoulder, .r1); bind(g.leftTrigger, .l2); bind(g.rightTrigger, .r2)
        bind(g.leftThumbstickButton, .l3); bind(g.rightThumbstickButton, .r3)
        // The on-screen PAUSE button is hidden while a controller is connected, so by default the controller's Menu/Start button pauses.
        bind(g.buttonMenu, .start); bind(g.buttonOptions, .select)
        // The left stick follows the D-pad buttons.
        g.leftThumbstick.valueChangedHandler = { _, x, y in
            let c = GameControls.shared
            PadInput.set("stick.left",  c.controllerScan(.dpadLeft),  x < -0.4)
            PadInput.set("stick.right", c.controllerScan(.dpadRight), x > 0.4)
            PadInput.set("stick.up",    c.controllerScan(.dpadUp),    y > 0.4)
            PadInput.set("stick.down",  c.controllerScan(.dpadDown),  y < -0.4)
        }
    }

    // Several physical controls can end up on the same game key (e.g. D-pad + stick), so presses are counted per key.
    private static var held: [String: Int32] = [:]   // control -> key it is holding down
    private static var counts: [Int32: Int] = [:]    // key -> how many controls hold it

    static func set(_ source: String, _ scan: Int32, _ pressed: Bool) {
        if pressed {
            guard scan != 0, held[source] == nil else { return }
            held[source] = scan
            counts[scan, default: 0] += 1
            if counts[scan] == 1 { rbo_set_key(scan, 1) }
        } else if let s = held.removeValue(forKey: source) {
            let left = (counts[s] ?? 1) - 1
            if left <= 0 { counts[s] = nil; rbo_set_key(s, 0) } else { counts[s] = left }
        }
    }

    /// On-screen touch D-pad (x: -1 left … 1 right, y: -1 down … 1 up) -> the keys assigned to the four move actions.
    static func axis(_ x: Float, _ y: Float) {
        let c = GameControls.shared
        rbo_set_key(c.scan(.left), x < -0.4 ? 1 : 0); rbo_set_key(c.scan(.right), x > 0.4 ? 1 : 0)
        rbo_set_key(c.scan(.up), y > 0.4 ? 1 : 0);    rbo_set_key(c.scan(.down), y < -0.4 ? 1 : 0)
    }

    static func releaseAll() {
        for source in Array(held.keys) { set(source, 0, false) }
        let c = GameControls.shared
        for action in PadAction.allCases { rbo_set_key(c.scan(action), 0) }   // touch pad / anything else still holding a key
    }
}
