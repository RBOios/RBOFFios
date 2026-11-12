import GameController

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
    static func axis(_ x: Float, _ y: Float) {
        rbo_set_key(DIK.left, x < -0.4 ? 1 : 0); rbo_set_key(DIK.right, x > 0.4 ? 1 : 0)
        rbo_set_key(DIK.up, y > 0.4 ? 1 : 0);    rbo_set_key(DIK.down, y < -0.4 ? 1 : 0)
    }
}
