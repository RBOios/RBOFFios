import GameController
import Combine

/// Physical gamepads (GCController) -> DirectInput keys.
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
        g.dpad.valueChangedHandler = { _, x, y in PadInput.axis(x, y) }
        g.leftThumbstick.valueChangedHandler = { _, x, y in PadInput.axis(x, y) }
        g.buttonA.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.a, p ? 1 : 0) }
        g.buttonB.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.s, p ? 1 : 0) }
        g.buttonX.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.d, p ? 1 : 0) }
        g.buttonY.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.f, p ? 1 : 0) }
        // The on-screen PAUSE button is hidden while a controller is connected, so the controller's Menu/Start button pauses instead.
        g.buttonMenu.pressedChangedHandler = { _, _, p in rbo_set_key(DIK.one, p ? 1 : 0) }
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
