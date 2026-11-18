import Foundation

/// The only supported executable: Expansion 3 (RBO_Ex3.exe).
enum Edition: String, CaseIterable, Identifiable {
    case ex3
    var id: String { rawValue }
    static let available: [Edition] = [.ex3]
    var title: String { tr("확장팩 3") }
    var exeName: String { "RBO_Ex3.exe" }
}

/// Clears the 2P/3P keyboard assignments (KeyConfig.cdt/.v2/.v3/.v4), keeping a .bak of the originals; also writes player 1's keys (see applyPlayerOne).
enum KeyConfigPatcher {
    static func apply(root: URL) {
        let dir = root.appendingPathComponent("DATA/SaveData")
        for name in ["KeyConfig.cdt", "KeyConfig.v2", "KeyConfig.v3", "KeyConfig.v4"] {
            let url = dir.appendingPathComponent(name)
            guard var data = try? Data(contentsOf: url), data.count == 328 else { continue }
            let bak = url.appendingPathExtension("bak")
            if !FileManager.default.fileExists(atPath: bak.path) { try? data.write(to: bak) }
            for off in [0xB8, 0x124] {                      // P2 and P3 key blocks, 9 dwords each
                for i in 0..<36 { data[off + i] = 0 }
            }
            try? data.write(to: url)
        }
    }

    /// Writes player 1's game keys (the launcher's 컨트롤 설정) into DATA/SaveData/KeyConfig.v4, so the game listens to the same keys the touch pad and
    /// gamepad send. File layout: UInt32 version (0), then 3 players x 108 bytes = 9 input types + 9 device indexes + 9 scan codes (UInt32 each, little endian).
    /// A missing file is created with only player 1 assigned; an existing file only gets player 1's block changed (a .before-p1.bak copy is kept once).
    static func applyPlayerOne(root: URL, controls: GameControls) {
        let fileSize = 328, playerSize = 108
        let dir = root.appendingPathComponent("DATA/SaveData")
        let url = dir.appendingPathComponent("KeyConfig.v4")
        let fm = FileManager.default
        let existed = fm.fileExists(atPath: url.path)
        let original: Data
        if existed {
            guard let d = try? Data(contentsOf: url), d.count == fileSize else { return }   // unknown format: leave the player's file alone
            original = d
        } else {
            original = Data(count: fileSize)
        }
        guard original.prefix(4).allSatisfy({ $0 == 0 }) else { return }
        var data = original
        func put(_ offset: Int, _ value: UInt32) {
            for i in 0..<4 { data[offset + i] = UInt8((value >> (8 * UInt32(i))) & 0xFF) }
        }
        let players = existed ? 1 : 3        // from scratch: the other two keyboards stay unassigned
        for player in 0..<players {
            let base = 4 + player * playerSize
            for action in PadAction.allCases {
                let field = action.rawValue * 4
                put(base + field, 1)                                                    // input type: keyboard
                put(base + 36 + field, 0)                                               // device index
                put(base + 72 + field, player == 0 ? UInt32(controls.scan(action)) : 0)  // scan code
            }
        }
        if existed && data == original { return }
        if existed {
            let bak = url.appendingPathExtension("before-p1.bak")
            if !fm.fileExists(atPath: bak.path) { try? original.write(to: bak) }
        } else {
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        }
        try? data.write(to: url, options: .atomic)
    }
}

/// "화면 크기" choices of the original RBO_Ex3_Config.exe (stored in DATA/SaveData/SystemConfig.v4).
enum ScreenSize: Int, CaseIterable, Identifiable {
    case s640 = 0, psp = 1, s800 = 2, s1024 = 3
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .s640: return "640x480"
        case .psp: return tr("가상 PSP")
        case .s800: return "800x600"
        case .s1024: return "1024x768"
        }
    }
    /// Shape of the picture on screen. The game always draws 640x480 (4:3); only the virtual PSP shows a 480x272 (16:9) window.
    var aspect: Float {
        switch self {
        case .psp: return 480.0 / 272.0
        default: return 4.0 / 3.0
        }
    }
}

/// Reads/writes the screen-size field of SystemConfig.v4 (what RBO_Ex3_Config.exe's "화면 크기" box saves).
/// File layout: UInt32 version (3) + 19 UInt32 settings (76 bytes); the screen size is the 2nd setting (file offset 8).
enum SystemConfigPatcher {
    private static let fileSize = 80
    private static let sizeOffset = 8
    /// Defaults of RBO_Ex3_Config.exe, used only when the file does not exist yet.
    private static let defaults: [UInt32] = [1, 0, 1, 0, 0, 16, 16, 16, 16, 16, 16, 2, 2, 1, 0, 60, 0, 0, 0]

    private static func fileURL(_ root: URL) -> URL {
        root.appendingPathComponent("DATA/SaveData/SystemConfig.v4")
    }

    static func current(root: URL) -> ScreenSize? {
        guard let d = try? Data(contentsOf: fileURL(root)), d.count == fileSize, d[0] == 3 else { return nil }
        return ScreenSize(rawValue: Int(d[sizeOffset]))
    }

    static func apply(root: URL, size: ScreenSize) {
        let url = fileURL(root)
        if var d = try? Data(contentsOf: url) {
            guard d.count == fileSize, d[0] == 3 else { return }       // unknown/older format: leave the player's file alone
            d[sizeOffset] = UInt8(size.rawValue); d[sizeOffset + 1] = 0; d[sizeOffset + 2] = 0; d[sizeOffset + 3] = 0
            try? d.write(to: url)
        } else {
            var vals = defaults; vals[1] = UInt32(size.rawValue)
            var d = Data(); var ver = UInt32(3).littleEndian; d.append(Data(bytes: &ver, count: 4))
            for v in vals { var x = v.littleEndian; d.append(Data(bytes: &x, count: 4)) }
            try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? d.write(to: url)
        }
    }
}
