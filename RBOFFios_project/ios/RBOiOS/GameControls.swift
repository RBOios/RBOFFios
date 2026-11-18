import Foundation

/// The nine player-1 actions, in the order the game's KeyConfig.v4 stores them.
enum PadAction: Int, CaseIterable, Identifiable {
    case up, down, left, right, weak, strong, guardBtn, special, pause
    var id: Int { rawValue }

    var title: String {
        switch self {
        case .up:       return tr("위로 이동")
        case .down:     return tr("아래로 이동")
        case .left:     return tr("왼쪽으로 이동")
        case .right:    return tr("오른쪽으로 이동")
        case .weak:     return tr("약 공격").replacingOccurrences(of: "\n", with: " ")    // 터치 버튼용 두 줄 이름을 한 줄로
        case .strong:   return tr("강 공격").replacingOccurrences(of: "\n", with: " ")
        case .guardBtn: return tr("방어")
        case .special:  return tr("특수")
        case .pause:    return tr("일시정지")
        }
    }
    /// DirectInput scan code the game uses by default for this action.
    var defaultScan: Int32 {
        switch self {
        case .up: return 0xC8
        case .down: return 0xD0
        case .left: return 0xCB
        case .right: return 0xCD
        case .weak: return 0x1E
        case .strong: return 0x1F
        case .guardBtn: return 0x20
        case .special: return 0x21
        case .pause: return 0x02
        }
    }
    var defaultButton: ControllerButton {
        switch self {
        case .up: return .dpadUp
        case .down: return .dpadDown
        case .left: return .dpadLeft
        case .right: return .dpadRight
        case .weak: return .a
        case .strong: return .b
        case .guardBtn: return .x
        case .special: return .y
        case .pause: return .start
        }
    }
}

/// Physical gamepad controls that can be assigned to an action.
enum ControllerButton: Int, CaseIterable, Identifiable {
    case dpadUp, dpadDown, dpadLeft, dpadRight, a, b, x, y, l1, r1, l2, r2, l3, r3, start, select
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .dpadUp: return tr("십자키 위")
        case .dpadDown: return tr("십자키 아래")
        case .dpadLeft: return tr("십자키 왼쪽")
        case .dpadRight: return tr("십자키 오른쪽")
        case .a: return "A"
        case .b: return "B"
        case .x: return "X"
        case .y: return "Y"
        case .l1: return "L1"
        case .r1: return "R1"
        case .l2: return "L2"
        case .r2: return "R2"
        case .l3: return "L3"
        case .r3: return "R3"
        case .start: return "Start"
        case .select: return "Select"
        }
    }
}

/// A keyboard key the game can be told to listen to (DirectInput scan code).
struct GameKey: Identifiable {
    let name: String
    let scan: Int32
    var id: Int32 { scan }

    static var all: [GameKey] {
        var keys = [GameKey(name: tr("위"), scan: 0xC8), GameKey(name: tr("아래"), scan: 0xD0),
                    GameKey(name: tr("왼쪽"), scan: 0xCB), GameKey(name: tr("오른쪽"), scan: 0xCD)]
        let letterScans: [Int32] = [0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                    0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c]   // A ... Z
        for (i, scan) in letterScans.enumerated() {
            keys.append(GameKey(name: String(UnicodeScalar(UInt8(65 + i))), scan: scan))
        }
        for d in 0...9 { keys.append(GameKey(name: String(d), scan: d == 0 ? 0x0b : Int32(d + 1))) }
        return keys
    }
    static func name(for scan: Int32) -> String { all.first { $0.scan == scan }?.name ?? "?" }
}

/// Player 1's bindings: which game key each action sends, and which gamepad button triggers it.
/// Saved in UserDefaults; the game keys are also written into the game's KeyConfig.v4 when the game starts.
final class GameControls: ObservableObject {
    static let shared = GameControls()
    private static let storeKey = "playerOneControls.v1"
    private static let formatVersion = 1

    @Published private(set) var scans: [Int32]
    @Published private(set) var buttons: [ControllerButton]

    private static var defaults: ([Int32], [ControllerButton]) {
        (PadAction.allCases.map { $0.defaultScan }, PadAction.allCases.map { $0.defaultButton })
    }

    private init() {
        let loaded = GameControls.load()
        scans = loaded.0; buttons = loaded.1
    }

    private static func load() -> ([Int32], [ControllerButton]) {
        let count = PadAction.allCases.count
        guard let raw = UserDefaults.standard.array(forKey: storeKey) as? [Int],
              raw.count == 1 + 2 * count, raw[0] == formatVersion else { return defaults }
        let valid = Set(GameKey.all.map { $0.scan })
        var s: [Int32] = [], b: [ControllerButton] = []
        for i in 0..<count {
            let scan = Int32(truncatingIfNeeded: raw[1 + 2 * i])
            guard valid.contains(scan), let button = ControllerButton(rawValue: raw[2 + 2 * i]),
                  !s.contains(scan), !b.contains(button) else { return defaults }
            s.append(scan); b.append(button)
        }
        return (s, b)
    }

    private func save() {
        var raw: [Int] = [GameControls.formatVersion]
        for i in 0..<PadAction.allCases.count { raw.append(Int(scans[i])); raw.append(buttons[i].rawValue) }
        UserDefaults.standard.set(raw, forKey: GameControls.storeKey)
    }

    func scan(_ action: PadAction) -> Int32 { scans[action.rawValue] }
    func button(_ action: PadAction) -> ControllerButton { buttons[action.rawValue] }

    /// Scan code of the action this gamepad button is assigned to (0 = not assigned to anything).
    func controllerScan(_ button: ControllerButton) -> Int32 {
        if let i = buttons.firstIndex(of: button) { return scans[i] }
        return 0
    }

    /// Choosing a key that another action already uses swaps the two.
    func assignKey(_ action: PadAction, _ scan: Int32) {
        let target = action.rawValue
        for i in scans.indices where i != target && scans[i] == scan { scans[i] = scans[target] }
        scans[target] = scan
        save()
    }

    func assignButton(_ action: PadAction, _ button: ControllerButton) {
        let target = action.rawValue
        for i in buttons.indices where i != target && buttons[i] == button { buttons[i] = buttons[target] }
        buttons[target] = button
        save()
    }

    func reset() {
        let d = GameControls.defaults
        scans = d.0; buttons = d.1
        save()
    }
}
