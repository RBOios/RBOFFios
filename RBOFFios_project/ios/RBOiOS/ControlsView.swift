import SwiftUI
import GameController

/// Listens for the next gamepad button press (or D-pad / left-stick move) so it can be assigned to an action.
final class ControllerCapture: ObservableObject {
    private var observer: NSObjectProtocol?
    private var hooked: [GCController] = []
    private var onPress: ((ControllerButton) -> Void)?

    func start(_ handler: @escaping (ControllerButton) -> Void) {
        stop()
        onPress = handler
        GCController.controllers().forEach(hook)
        observer = NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.hook(c) }
        }
        GCController.startWirelessControllerDiscovery(completionHandler: {})
    }

    func stop() {
        if let o = observer { NotificationCenter.default.removeObserver(o); observer = nil }
        for c in hooked { if let g = c.extendedGamepad { ControllerCapture.clear(g) } }
        hooked = []
        onPress = nil
    }

    private func fire(_ b: ControllerButton) {
        guard let handler = onPress else { return }
        stop()
        DispatchQueue.main.async { handler(b) }
    }

    private func hook(_ c: GCController) {
        guard let g = c.extendedGamepad else { return }
        hooked.append(c)
        func bind(_ input: GCControllerButtonInput?, _ b: ControllerButton) {
            input?.pressedChangedHandler = { [weak self] _, _, pressed in if pressed { self?.fire(b) } }
        }
        bind(g.dpad.up, .dpadUp); bind(g.dpad.down, .dpadDown); bind(g.dpad.left, .dpadLeft); bind(g.dpad.right, .dpadRight)
        bind(g.buttonA, .a); bind(g.buttonB, .b); bind(g.buttonX, .x); bind(g.buttonY, .y)
        bind(g.leftShoulder, .l1); bind(g.rightShoulder, .r1); bind(g.leftTrigger, .l2); bind(g.rightTrigger, .r2)
        bind(g.leftThumbstickButton, .l3); bind(g.rightThumbstickButton, .r3)
        bind(g.buttonMenu, .start); bind(g.buttonOptions, .select)
        g.leftThumbstick.valueChangedHandler = { [weak self] _, x, y in   // the left stick follows the D-pad
            guard abs(x) > 0.6 || abs(y) > 0.6 else { return }
            if abs(x) > abs(y) { self?.fire(x < 0 ? .dpadLeft : .dpadRight) } else { self?.fire(y > 0 ? .dpadUp : .dpadDown) }
        }
    }

    private static func clear(_ g: GCExtendedGamepad) {
        let inputs: [GCControllerButtonInput?] = [g.dpad.up, g.dpad.down, g.dpad.left, g.dpad.right,
                                                  g.buttonA, g.buttonB, g.buttonX, g.buttonY,
                                                  g.leftShoulder, g.rightShoulder, g.leftTrigger, g.rightTrigger,
                                                  g.leftThumbstickButton, g.rightThumbstickButton, g.buttonMenu, g.buttonOptions]
        for i in inputs { i?.pressedChangedHandler = nil }
        g.leftThumbstick.valueChangedHandler = nil
    }
}

/// "Player 1 controls": for every action, pick the game key it sends and the gamepad button that triggers it.
/// Styled like the iOS Settings app: inset grouped list, nav bar with a centred title, pull-down menus.
struct ControlsView: View {
    @Environment(\.presentationMode) private var presentation
    @ObservedObject private var controls = GameControls.shared
    @StateObject private var capture = ControllerCapture()
    @State private var capturing: PadAction? = nil
    @State private var confirmReset = false

    private let pillWidth: CGFloat = 132

    var body: some View {
        ZStack {
            Color(.systemGroupedBackground).ignoresSafeArea()
            VStack(spacing: 0) {
                navBar
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        Text(tr("왼쪽 스틱은 십자키를 따라가요. 이미 쓰고 있는 키나 버튼을 고르면 두 동작이 서로 바뀌어요."))
                            .font(.footnote).foregroundColor(.secondary).padding(.horizontal, 16)
                        HStack(spacing: 8) {
                            Spacer()
                            Text(tr("게임 키")).frame(width: pillWidth)
                            Text(tr("컨트롤러")).frame(width: pillWidth)
                        }
                        .font(.footnote).foregroundColor(.secondary).padding(.horizontal, 16)
                        VStack(spacing: 0) {
                            ForEach(PadAction.allCases) { action in
                                row(action)
                                if action != PadAction.allCases.last { Divider().padding(.leading, 16) }
                            }
                        }
                        .background(Color(.secondarySystemGroupedBackground))
                        .cornerRadius(10)
                    }
                    .padding(.horizontal, 16).padding(.top, 10).padding(.bottom, 24)
                }
            }

            if let action = capturing {
                ZStack {
                    Color.black.opacity(0.4).ignoresSafeArea()
                    VStack(spacing: 0) {
                        VStack(spacing: 6) {
                            Text(action.title).font(.system(size: 17, weight: .semibold))
                            Text(tr("컨트롤러의 버튼을 누르거나 십자키·왼쪽 스틱을 움직여 주세요."))
                                .font(.footnote).multilineTextAlignment(.center)
                        }
                        .foregroundColor(.primary).padding(20)
                        Divider()
                        Button(action: { stopCapture() }) {
                            Text(tr("취소")).font(.system(size: 17)).frame(maxWidth: .infinity, minHeight: 44)
                        }
                    }
                    .frame(maxWidth: 300)
                    .background(.regularMaterial).cornerRadius(14)
                }
            }
        }
        .preferredColorScheme(.dark)
        .accentColor(.cyan)
        .alert(tr("기본 키와 컨트롤러 버튼으로 되돌릴까요?"), isPresented: $confirmReset) {
            Button(tr("되돌리기"), role: .destructive) { controls.reset() }
            Button(tr("취소"), role: .cancel) {}
        }
        .onDisappear { capture.stop() }
    }

    /// Top bar: "기본값으로" (red, left) - title (centre) - "완료" (bold, right), with a hairline under it.
    private var navBar: some View {
        VStack(spacing: 0) {
            ZStack {
                Text(tr("플레이어 1 컨트롤")).font(.system(size: 17, weight: .semibold)).foregroundColor(.primary)
                HStack {
                    Button(action: { ClickSound.play(); confirmReset = true }) {
                        Text(tr("기본값으로")).font(.system(size: 17)).foregroundColor(.red)
                    }
                    Spacer()
                    Button(action: { ClickSound.play(); stopCapture(); presentation.wrappedValue.dismiss() }) {
                        Text(tr("완료")).font(.system(size: 17, weight: .semibold))
                    }
                }
            }
            .padding(.horizontal, 16).frame(height: 44)
            Divider()
        }
        .background(Color(.secondarySystemBackground).ignoresSafeArea(edges: .top))
    }

    /// The pull-down button: current value in a rounded fill (no arrows).
    private func pill(_ text: String) -> some View {
        Text(text).lineLimit(1).minimumScaleFactor(0.7)
        .font(.system(size: 16)).foregroundColor(.accentColor)
        .frame(width: pillWidth, height: 32)
        .background(Color(.tertiarySystemFill)).cornerRadius(8)
    }

    private func row(_ action: PadAction) -> some View {
        HStack(spacing: 8) {
            Text(action.title).font(.system(size: 17)).foregroundColor(.primary).lineLimit(1).minimumScaleFactor(0.8)
            Spacer(minLength: 8)
            Menu {
                ForEach(GameKey.all) { key in
                    Button(action: { controls.assignKey(action, key.scan) }) {
                        if controls.scan(action) == key.scan { Label(key.name, systemImage: "checkmark") } else { Text(key.name) }
                    }
                }
            } label: { pill(GameKey.name(for: controls.scan(action))) }
            Menu {
                Button(tr("컨트롤러 버튼 누르기…")) { startCapture(action) }
                Divider()
                ForEach(ControllerButton.allCases) { button in
                    Button(action: { controls.assignButton(action, button) }) {
                        if controls.button(action) == button { Label(button.title, systemImage: "checkmark") } else { Text(button.title) }
                    }
                }
            } label: { pill(controls.button(action).title) }
        }
        .padding(.horizontal, 16).frame(minHeight: 48)
    }

    private func startCapture(_ action: PadAction) {
        capturing = action
        capture.start { button in
            controls.assignButton(action, button)
            capturing = nil
        }
    }

    private func stopCapture() {
        capture.stop()
        capturing = nil
    }
}
