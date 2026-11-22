import SwiftUI
import UIKit

// MARK: - 터치 입력 (한 곳에서 모든 손가락을 처리)
//
// 예전에는 D-pad / 버튼 4개가 각자 DragGesture 를 따로 가지고 있었음 → 손가락이 처음 닿은 버튼에만 묶이고,
// 버튼 사이 빈틈이나 원 바깥을 누르면 아무 반응이 없었음.
// 이제는 UIKit 멀티터치 레이어 하나가 모든 손가락을 받아서 "가장 가까운 컨트롤"에 연결함:
//  - 버튼 사이 빈틈 / 원 바깥 가장자리를 눌러도 가까운 버튼이 눌림 (감지 범위 넓힘)
//  - 손가락을 버튼 위에서 미끄러뜨리면 버튼이 바뀜 (강 공격 → 약 공격 등)
//  - D-pad 는 그림보다 넓은 범위에서 인식, 중앙 무반응 구역(deadzone) 축소
//  - 시스템이 터치를 가로채도(touchesCancelled) 눌림이 남지 않고 정리됨
//  - 같은 버튼을 아주 빠르게 연타해도 두 번째 입력이 게임에 따로 전달됨

/// D-pad 네 방향의 눌림 / 불 켜짐 상태
struct PadDir: Equatable {
    var l = false, r = false, u = false, d = false
    subscript(i: Int) -> Bool {
        get { [l, r, u, d][i] }
        set { switch i { case 0: l = newValue; case 1: r = newValue; case 2: u = newValue; default: d = newValue } }
    }
}

/// 터치 컨트롤의 화면 좌표 배치 (VirtualPad 가 계산해서 터치 레이어에 넘겨줌)
struct PadLayout {
    var dpadCenter = CGPoint.zero
    var dpadSize: CGFloat = 0                                   // 그려진 D-pad 한 변 길이
    var buttonCenters = [CGPoint](repeating: .zero, count: 4)   // 특수, 방어, 강 공격, 약 공격 순서
    var buttonSpacing: CGFloat = 0                              // 이웃한 두 버튼 중심 사이 거리

    // ===== 터치 감도 조절 (여기 숫자만 바꾸면 됨) =====
    /// D-pad 터치 인식 반경 = 그려진 D-pad 반지름의 몇 배인가 (1.0 = 그림 크기 그대로)
    static let dpadCapture: CGFloat = 1.6
    /// 버튼 터치 인식 반경 = 이웃 버튼 중심 사이 거리의 몇 배인가 (0.5 = 빈틈 없이 딱 맞닿음, 클수록 바깥까지 인식)
    static let buttonCapture: CGFloat = 0.78

    var dpadReach: CGFloat { dpadSize / 2 * PadLayout.dpadCapture }
    var buttonReach: CGFloat { buttonSpacing * PadLayout.buttonCapture }

    func inDpad(_ p: CGPoint) -> Bool {
        hypot(p.x - dpadCenter.x, p.y - dpadCenter.y) <= dpadReach
    }

    /// 터치 위치에서 가장 가까운 버튼 번호 (인식 반경 밖이면 nil)
    func nearestButton(_ p: CGPoint) -> Int? {
        var best = -1
        var bestD = CGFloat.greatestFiniteMagnitude
        for (i, c) in buttonCenters.enumerated() {
            let d = hypot(p.x - c.x, p.y - c.y)
            if d < bestD { bestD = d; best = i }
        }
        return (best >= 0 && bestD <= buttonReach) ? best : nil
    }
}

/// 손가락 -> 게임 키 변환, 불 켜짐 상태 관리
final class PadTouchController: ObservableObject {
    // ===== 입력 동작 조절 =====
    static let deadzone: CGFloat = 0.08          // D-pad 중앙에서 아무것도 안 눌리는 구역 (반지름 비율, 예전 0.12)
    static let sector: CGFloat = 0.38            // sin(22.5°): 8방향 구분 기준
    static let minHold: TimeInterval = 0.10      // 짧게 톡 쳐도 키를 최소 이만큼 눌러 둠 (게임이 놓치지 않게)
    static let reGap: TimeInterval = 0.05        // 같은 버튼을 연타할 때 키가 "떼어져 있는" 최소 시간 (이게 있어야 2번째 입력이 따로 인식됨)
    static let minLit: TimeInterval = 0.18       // 불빛 최소 유지 시간
    static let actions: [PadAction] = [.special, .guardBtn, .strong, .weak]   // PadLayout.buttonCenters 와 같은 순서

    @Published private(set) var dirLit = PadDir()
    @Published private(set) var litButtons = [Bool](repeating: false, count: 4)

    // D-pad
    private var dir = PadDir()                                   // 실제 눌린 방향 (게임 입력)
    private var dirPressedAt = [Date](repeating: .distantPast, count: 4)
    private var dirTokens = [Int](repeating: 0, count: 4)
    private var dpadTouch: ObjectIdentifier? = nil               // D-pad 를 잡고 있는 손가락
    // 버튼: 손가락 -> 버튼 번호 (-1 = 어느 버튼 위도 아님)
    private var buttonTouch: [ObjectIdentifier: Int] = [:]
    private var want = [Bool](repeating: false, count: 4)        // 손가락이 지금 그 버튼 위에 있는가
    private var owed = [Bool](repeating: false, count: 4)        // 아직 게임에 전달 못 한 "새로 누름"이 있는가 (연타 보호)
    private var keyDown = [Bool](repeating: false, count: 4)     // 게임에 키를 눌러 둔 상태인가
    private var heldKey = [Int32](repeating: 0, count: 4)
    private var downAt = [Date](repeating: .distantPast, count: 4)
    private var lastUp = [Date](repeating: .distantPast, count: 4)
    private var litUntil = [Date](repeating: .distantPast, count: 4)

    private func later(_ s: TimeInterval, _ f: @escaping () -> Void) {
        DispatchQueue.main.asyncAfter(deadline: .now() + max(0, s), execute: f)
    }

    // MARK: 터치 이벤트 (PadTouchView 에서 호출)
    func touchBegan(_ id: ObjectIdentifier, at p: CGPoint, layout L: PadLayout) {
        if dpadTouch == nil && L.inDpad(p) {
            dpadTouch = id
            updateDpad(p, L)
            return
        }
        buttonTouch[id] = L.nearestButton(p) ?? -1
        refreshButtons()
    }

    func touchMoved(_ id: ObjectIdentifier, at p: CGPoint, layout L: PadLayout) {
        if id == dpadTouch {
            updateDpad(p, L)
        } else if buttonTouch[id] != nil {
            buttonTouch[id] = L.nearestButton(p) ?? -1   // 손가락이 미끄러져 다른 버튼으로 넘어가면 그 버튼이 눌림
            refreshButtons()
        }
    }

    func touchEnded(_ id: ObjectIdentifier) {
        if id == dpadTouch {
            dpadTouch = nil
            setDir(PadDir())
        } else if buttonTouch.removeValue(forKey: id) != nil {
            refreshButtons()
        }
    }

    /// 화면에서 사라질 때 / 게임패드가 연결될 때: 눌려 있던 것을 모두 뗌
    func releaseAll() {
        dpadTouch = nil
        buttonTouch.removeAll()
        setDir(PadDir())
        for i in 0..<4 {
            want[i] = false; owed[i] = false
            if keyDown[i] { rbo_set_key(heldKey[i], 0); keyDown[i] = false; lastUp[i] = Date() }
            litUntil[i] = .distantPast
            if litButtons[i] { litButtons[i] = false }
        }
    }

    // MARK: D-pad
    private func updateDpad(_ p: CGPoint, _ L: PadLayout) {
        let dx = p.x - L.dpadCenter.x, dy = p.y - L.dpadCenter.y
        let radius = L.dpadSize / 2
        let dist = hypot(dx, dy)
        var nd = PadDir()
        if dist > radius * PadTouchController.deadzone && dist > 0 {
            let nx = dx / dist, ny = dy / dist   // screen Y is down
            nd.l = nx < -PadTouchController.sector; nd.r = nx > PadTouchController.sector
            nd.u = ny < -PadTouchController.sector; nd.d = ny > PadTouchController.sector
        }
        setDir(nd)
    }

    private func setDir(_ nd: PadDir) {
        guard nd != dir else { return }
        let old = dir
        dir = nd
        for i in 0..<4 {
            if nd[i] && !old[i] {
                dirLit[i] = true; dirPressedAt[i] = Date(); dirTokens[i] += 1
            } else if !nd[i] && old[i] {
                let id = dirTokens[i]
                let wait = max(0, PadTouchController.minLit - Date().timeIntervalSince(dirPressedAt[i]))
                later(wait) { [weak self] in
                    guard let self = self else { return }
                    if self.dirTokens[i] == id && !self.dir[i] { self.dirLit[i] = false }   // 그 사이 다시 눌렸으면 끄지 않음
                }
            }
        }
        PadInput.axis(nd.l ? -1 : (nd.r ? 1 : 0), nd.u ? 1 : (nd.d ? -1 : 0))   // pad Y is up
    }

    // MARK: 버튼
    private func refreshButtons() {
        var counts = [Int](repeating: 0, count: 4)
        for b in buttonTouch.values where b >= 0 { counts[b] += 1 }
        for i in 0..<4 { setWant(i, counts[i] > 0) }
    }

    private func setWant(_ i: Int, _ on: Bool) {
        guard want[i] != on else { return }
        want[i] = on
        if on {
            if keyDown[i] { owed[i] = true }   // 이전 입력이 아직 안 떼어진 상태에서 또 눌림 -> 뗀 다음 다시 눌러서 별개의 입력으로 전달
            litUntil[i] = Date().addingTimeInterval(PadTouchController.minLit)
        }
        syncKey(i)
        syncLit(i)
    }

    /// 손가락 상태(want / owed)에 맞춰 게임 키를 누르거나 뗌. 몇 번을 불러도 안전함.
    private func syncKey(_ i: Int) {
        let now = Date()
        if keyDown[i] {
            guard !want[i] || owed[i] else { return }   // 손가락이 누르고 있는 동안은 그대로 유지
            let wait = downAt[i].addingTimeInterval(PadTouchController.minHold).timeIntervalSince(now)
            if wait > 0 { later(wait) { [weak self] in self?.syncKey(i) }; return }
            keyDown[i] = false; lastUp[i] = now
            rbo_set_key(heldKey[i], 0)
        }
        if want[i] || owed[i] {
            let wait = lastUp[i].addingTimeInterval(PadTouchController.reGap).timeIntervalSince(now)
            if wait > 0 { later(wait) { [weak self] in self?.syncKey(i) }; return }
            owed[i] = false
            let key = GameControls.shared.scan(PadTouchController.actions[i])
            keyDown[i] = true; downAt[i] = now; heldKey[i] = key
            rbo_set_key(key, 1)
            if !want[i] { later(PadTouchController.minHold) { [weak self] in self?.syncKey(i) } }   // 이미 손가락을 뗐다면 최소 시간 뒤에 뗌
        }
    }

    private func syncLit(_ i: Int) {
        let on = want[i] || Date() < litUntil[i]
        if litButtons[i] != on { litButtons[i] = on }
        if on && !want[i] {
            later(litUntil[i].timeIntervalSinceNow + 0.01) { [weak self] in self?.syncLit(i) }
        }
    }
}

/// 화면 전체 크기의 투명한 UIKit 멀티터치 레이어. D-pad / 버튼 인식 범위 안에서 시작된 터치만 받고, 나머지는 아래(SwiftUI 버튼 등)로 통과시킴.
final class PadTouchView: UIView {
    var layout = PadLayout()
    var controller: PadTouchController?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        isOpaque = false
        backgroundColor = .clear
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        layout.inDpad(point) || layout.nearestButton(point) != nil
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches { controller?.touchBegan(ObjectIdentifier(t), at: t.location(in: self), layout: layout) }
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches { controller?.touchMoved(ObjectIdentifier(t), at: t.location(in: self), layout: layout) }
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches { controller?.touchEnded(ObjectIdentifier(t)) }
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        for t in touches { controller?.touchEnded(ObjectIdentifier(t)) }
    }
}

struct PadTouchLayer: UIViewRepresentable {
    let layout: PadLayout
    let controller: PadTouchController
    func makeUIView(context: Context) -> PadTouchView {
        let v = PadTouchView()
        v.controller = controller
        v.layout = layout
        return v
    }
    func updateUIView(_ v: PadTouchView, context: Context) {
        v.controller = controller
        v.layout = layout
    }
}

// MARK: - 화면 구성

/// On-screen D-pad + action buttons + PAUSE button.
/// D-pad -> the 4 move keys (8 directions), the 4 round buttons -> 약 공격 / 강 공격 / 방어 / 특수, PAUSE image -> Pause.
/// Which key each one sends comes from GameControls (the launcher's 컨트롤 설정); the defaults are the arrows, A/S/D/F and "1".
/// D-pad 와 4개 버튼은 그림만 그리고, 터치는 PadTouchLayer(위)가 한꺼번에 처리함. PAUSE 는 예전 방식 그대로.
struct VirtualPad: View {
    // D-pad frame as fractions of the screen (x, y, width, height); the pad is drawn as the largest square that fits.
    private let dpadFrame = CGRect(x: 0.055, y: 0.560, width: 0.190, height: 0.315)
    // Game picture is drawn aspect-fit and centred in this shape (4:3 normally, 16:9 for the virtual PSP); the rest of the screen is black bars.
    private let gameAspect: CGFloat
    @State private var showCheat = false    // 치트 패널(PC 트레이너 기능)이 열려 있는지
    @StateObject private var touch = PadTouchController()
    // 블루투스/MFi 패드가 연결되면 true: D-pad / 4개 버튼 / PAUSE 는 숨기고, 치트 버튼과 패널은 그대로 둔다 (터치로 써야 하므로)
    private let controllerConnected: Bool
    init(gameAspect: CGFloat = 4.0 / 3.0, controllerConnected: Bool = false) {
        self.gameAspect = gameAspect
        self.controllerConnected = controllerConnected
    }
    // Round buttons, laid out in a diamond. Units: fractions of the screen height, relative to the cluster centre.
    // 순서는 PadTouchController.actions (특수, 방어, 강 공격, 약 공격) 와 같아야 함
    private let buttons: [(label: String, dx: CGFloat, dy: CGFloat)] = [
        (tr("특수"),   0.0,    -0.130),   // top
        (tr("방어"),   -0.1075, 0.0),     // left
        (tr("강 공격"), 0.1075,  0.0),     // right
        (tr("약 공격"), 0.0,     0.130),   // bottom
    ]
    private let buttonDiameter: CGFloat = 0.135   // of screen height
    private let clusterCentreY: CGFloat = 0.70    // of screen height
    private let pauseWidth: CGFloat = 0.20        // PAUSE button width, of screen height (the image keeps its own aspect ratio)
    private let pauseTopMargin: CGFloat = 0.05    // gap between PAUSE and the top edge, of screen height
    private let tabletScale: CGFloat = 1.3        // extra size of the whole touch UI on iPad (1.0 = phone-sized)
    private let tabletPadScale: CGFloat = 1.3     // iPad only: extra size of the D-pad and the 4 action buttons (PAUSE not included)
    private let tabletPadDown: CGFloat = 0.07     // iPad only: moves the D-pad and the 4 action buttons down, of screen height
    private let phoneButtonSpread: CGFloat = 1.15  // spacing between the 4 action buttons on iPhone AND iPad (1.0 = as before); button size is unchanged
    private let phonePadScale: CGFloat = 1.2      // iPhone only: extra size of the D-pad and the 4 action buttons together (the buttons follow the D-pad's size; PAUSE unchanged)

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            // Centre of the right-hand black bar, so the buttons stay off the game picture
            // (on screens with no bar, e.g. iPad, they just stay inside the right edge).
            let barWidth = max((w - gameAspect * h) / 2, 0)
            let clusterCX = min(w - barWidth / 2, w - 0.205 * h)
            // Everything is sized from the screen height, so on a tablet (about twice as tall as a phone) the pad would be twice as big.
            // `ui` shrinks the whole touch UI on iPad to roughly the phone's physical size, then `tabletScale` enlarges it (phones stay at 1).
            let isPad = UIDevice.current.userInterfaceIdiom == .pad
            let ui: CGFloat = isPad ? min(1, 400 / h) * tabletScale : 1
            let padUI: CGFloat = isPad ? ui * tabletPadScale : phonePadScale        // D-pad + action buttons
            let down: CGFloat = isPad ? tabletPadDown : 0
            // Scale the whole action-button cluster so its overall height equals the D-pad's drawn size.
            let dpadSize = min(dpadFrame.width * w, dpadFrame.height * h) * padUI
            let clusterExtent: CGFloat = 2 * 0.130 + buttonDiameter     // top edge of 특수 to bottom edge of 약 공격, of screen height
            let k = dpadSize / (clusterExtent * h)
            let spread: CGFloat = phoneButtonSpread   // 아이폰과 아이패드 모두 같은 간격 배율
            let btnD = buttonDiameter * h * k
            let nearest = CGFloat(hypot(0.1075, 0.130)) * h * k * spread                 // 이웃한 두 버튼 중심 사이 거리
            let dpadCentre = CGPoint(x: dpadFrame.midX * w, y: (dpadFrame.midY + down) * h)
            let centres: [CGPoint] = buttons.map { (b) -> CGPoint in
                let x: CGFloat = clusterCX + b.dx * h * k * spread
                let y: CGFloat = (clusterCentreY + down) * h + b.dy * h * k * spread
                return CGPoint(x: x, y: y)
            }
            let layout = PadLayout(dpadCenter: dpadCentre, dpadSize: dpadSize, buttonCenters: centres, buttonSpacing: nearest)
            ZStack {
                if !controllerConnected {
                // 맨 아래: 멀티터치 레이어 (D-pad / 버튼 터치를 전부 여기서 처리)
                PadTouchLayer(layout: layout, controller: touch)
                    .frame(width: w, height: h)
                DPad(lit: touch.dirLit)
                    .frame(width: dpadFrame.width * w * padUI, height: dpadFrame.height * h * padUI)
                    .position(x: dpadCentre.x, y: dpadCentre.y)
                    .allowsHitTesting(false)
                ForEach(0..<buttons.count, id: \.self) { i in
                    PadButton(label: buttons[i].label, lit: touch.litButtons[i], fontScale: 0.30)
                        .frame(width: btnD, height: btnD)
                        .position(x: centres[i].x, y: centres[i].y)
                        .allowsHitTesting(false)
                }
                PauseButton(key: GameControls.shared.scan(.pause))
                    .frame(width: pauseWidth * h * ui)
                    .position(x: clusterCX, y: pauseTopMargin * h + ((pauseWidth * h * ui - 20) * 0.392 + 20) / 2)   // top right, above the button cluster
                }   // !controllerConnected
                // 치트 버튼: PAUSE 바로 아래, 같은 크기(그림 비율도 같음)
                let pauseW = pauseWidth * h * ui
                let pauseH = (pauseW - 20) * 0.392 + 20
                CheatButton(open: showCheat)
                    .frame(width: pauseW)
                    .position(x: clusterCX, y: pauseTopMargin * h + pauseH + pauseH / 2 - 8)
                    .onTapGesture { showCheat.toggle() }
                if showCheat {
                    // 패널은 치트 버튼 왼쪽에 놓는 작은 카드 (게임은 계속 진행되고, 카드 밖 터치는 그대로 게임으로 전달됨)
                    let px = max(CheatPanel.width / 2 + 8, clusterCX - pauseW / 2 - 8 - CheatPanel.width / 2)
                    CheatPanel(close: { showCheat = false })
                        .position(x: px, y: max(CheatPanel.height / 2 + 6, pauseTopMargin * h + CheatPanel.height / 2))
                }
            }
            .frame(width: w, height: h)
            .onDisappear { touch.releaseAll() }
            .onChange(of: controllerConnected) { connected in
                if connected { touch.releaseAll() }   // 눌려 있던 터치 키가 남지 않도록
            }
        }
        .ignoresSafeArea()
    }
}

/// Four-piece D-pad (PPSSPP arrows: outline + a filled arrow that lights up while pressed; transparent background).
/// 그림만 담당. 어느 방향이 눌렸는지는 PadTouchController 가 계산해서 `lit` 으로 넘겨줌.
struct DPad: View {
    let lit: PadDir
    private static let idleOpacity: Double = 0.32     // pieces at rest (same as the action-button outline)
    private static let pressedOpacity: Double = 0.80  // pressed (same as the action buttons)
    private static let glowOpacity: Double = 0.50     // PPSSPP "glow": the filled arrow lights up behind the outline while pressed

    var body: some View {
        GeometryReader { geo in
            let size = min(geo.size.width, geo.size.height)
            ZStack {
                piece("DPadUp", 0, lit.u)
                piece("DPadDown", 180, lit.d)
                piece("DPadLeft", 270, lit.l)
                piece("DPadRight", 90, lit.r)
            }
            .frame(width: size, height: size)
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }

    private func piece(_ name: String, _ rotation: Double, _ pressed: Bool) -> some View {
        ZStack {
            ArrowGlow(rotation: rotation, glow: Self.glowOpacity)
                .opacity(pressed ? 1 : 0)
            Image(name)
                .resizable()
                .scaledToFit()
                .opacity(pressed ? Self.pressedOpacity : Self.idleOpacity)
        }
    }
}

/// PPSSPP "I_DIR" filled arrow (two overlapping translucent pentagons), drawn as vectors so it is always sharp.
/// The coordinates are the Up arm in a unit square; the other arms are rotated copies. It lights up behind the outline while a direction is pressed.
private struct ArrowGlow: View {
    let rotation: Double      // 0 = Up, 90 = Right, 180 = Down, 270 = Left
    let glow: Double          // PPSSPP: white at 50 % of the button opacity
    private static let outer = [CGPoint(x: 0.3623, y: 0.0285), CGPoint(x: 0.3623, y: 0.2912), CGPoint(x: 0.5002, y: 0.4115), CGPoint(x: 0.6377, y: 0.2912), CGPoint(x: 0.6377, y: 0.0285)]
    private static let inner = [CGPoint(x: 0.3924, y: 0.0586), CGPoint(x: 0.3924, y: 0.2713), CGPoint(x: 0.5018, y: 0.3688), CGPoint(x: 0.6108, y: 0.2713), CGPoint(x: 0.6108, y: 0.0586)]

    var body: some View {
        // PPSSPP's picture has alpha 0.67 and 0.89 where the two pentagons overlap; scale both by `glow`
        let a = 0.67 * glow, b = 0.89 * glow
        let innerAlpha = 1 - (1 - b) / (1 - a)
        return GeometryReader { g in
            ZStack {
                shape(Self.outer, g.size).fill(Color.white.opacity(a))
                shape(Self.inner, g.size).fill(Color.white.opacity(innerAlpha))
            }
        }
        .rotationEffect(.degrees(rotation))
    }

    private func shape(_ pts: [CGPoint], _ s: CGSize) -> Path {
        Path { p in
            p.move(to: CGPoint(x: pts[0].x * s.width, y: pts[0].y * s.height))
            for q in pts.dropFirst() { p.addLine(to: CGPoint(x: q.x * s.width, y: q.y * s.height)) }
            p.closeSubpath()
        }
    }
}

/// PPSSPP "I_RECT" filled rectangle (two overlapping translucent rectangles) centred on the PAUSE outline; drawn as vectors so it is always sharp.
private struct RectGlow: View {
    let glow: Double          // PPSSPP: white at 50 % of the button opacity
    var body: some View {
        // PPSSPP's picture has alpha 0.67 and 0.89 where the two rectangles overlap; scale both by `glow`
        let a = 0.67 * glow, b = 0.89 * glow
        let innerAlpha = 1 - (1 - b) / (1 - a)
        return GeometryReader { g in
            ZStack {
                Rectangle().fill(Color.white.opacity(a))
                    .frame(width: g.size.width * 0.9616, height: g.size.height * 0.9258)
                Rectangle().fill(Color.white.opacity(innerAlpha))
                    .frame(width: g.size.width * 0.8600, height: g.size.height * 0.6564)
            }
            .frame(width: g.size.width, height: g.size.height)
        }
    }
}

/// PAUSE image button (PPSSPP Start-button look: rectangle outline + label; transparent background). Sends the Pause key. A short tap is held for at least 0.1 s so the game never misses it.
struct PauseButton: View {
    let key: Int32
    @State private var held = false
    @State private var lit = false          // 화면에 불 켜진 모양 (손가락을 금방 떼도 최소 시간은 유지)
    @State private var pressedAt = Date()
    @State private var token = 0

    var body: some View {
        ZStack {
            RectGlow(glow: 0.5)                // PPSSPP Start-button glow: the filled rectangle lights up while pressed
                .opacity(lit ? 1 : 0)
            Image("PauseButton")
                .resizable()
                .scaledToFit()
                .opacity(lit ? 0.80 : 0.32)   // same as the action-button outline (idle 0.32, pressed 0.80)
        }
        .aspectRatio(1264.0 / 496.0, contentMode: .fit)   // = the picture's shape, so the glow lines up with it
            .padding(10)                       // bigger touch target than the picture itself
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !held else { return }
                    held = true; lit = true; pressedAt = Date(); token += 1
                    rbo_set_key(key, 1)
                }
                .onEnded { _ in
                    held = false
                    let id = token
                    let elapsed = Date().timeIntervalSince(pressedAt)
                    // 키는 최소 0.10초, 불빛은 최소 0.18초 유지 (짧게 톡 쳐도 불이 들어온 게 보이도록)
                    DispatchQueue.main.asyncAfter(deadline: .now() + max(0, 0.10 - elapsed)) {
                        if token == id { rbo_set_key(key, 0) }   // ignore if pressed again meanwhile
                    }
                    DispatchQueue.main.asyncAfter(deadline: .now() + max(0, 0.18 - elapsed)) {
                        if token == id { lit = false }
                    }
                })
    }
}

/// Round button with a text label. 그림만 담당 (터치는 PadTouchController). `lit` = 눌려서 불이 켜진 상태.
struct PadButton: View {
    let label: String
    let lit: Bool
    var fontScale: CGFloat = 0.30

    var body: some View {
        GeometryReader { geo in
            let m = max(min(geo.size.width, geo.size.height), 1)
            ZStack {
                Capsule().fill(Color.white.opacity(lit ? 0.30 : 0.08))
                Capsule().stroke(Color.white.opacity(lit ? 0.80 : 0.32), lineWidth: lit ? 3 : 1.5)
                    .shadow(color: Color.white.opacity(lit ? 0.70 : 0), radius: lit ? 8 : 0)   // lights up when pressed, like the D-pad
                // 두 줄짜리 이름(영어 Weak/Strong Attack)은 원 위·아래쪽으로 갈수록 폭이 좁아져서 글자가 삐져나옴 -> 글자를 줄이고 폭을 원 안쪽으로 제한
                let twoLine = label.contains("\n")
                Text(label)
                    .font(.system(size: m * fontScale * 0.80, weight: .bold))
                    .lineLimit(twoLine ? 2 : 1)
                    .multilineTextAlignment(.center)
                    .minimumScaleFactor(0.5)
                    .frame(width: m * (twoLine ? 0.80 : 0.84))
                    .foregroundColor(Color.white.opacity(lit ? 0.80 : 0.32))   // same as the D-pad (idle 0.32, pressed 0.80)
            }
            .frame(width: m, height: m)
            .frame(width: geo.size.width, height: geo.size.height)
        }
    }
}
