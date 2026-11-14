import Foundation

/// The four executables that ship with the game folder.
enum Edition: String, CaseIterable, Identifiable {
    case original, ex1, ex2, ex3
    var id: String { rawValue }
    /// Editions shown in the launcher (only Expansion 3 for now).
    static let available: [Edition] = [.ex3]
    var title: String {
        switch self {
        case .original: return "오리지널"
        case .ex1: return "확장팩 1"
        case .ex2: return "확장팩 2"
        case .ex3: return "확장팩 3"
        }
    }
    var exeName: String {
        switch self {
        case .original: return "RBO.exe"
        case .ex1: return "RBO_Ex1.exe"
        case .ex2: return "RBO_Ex2.exe"
        case .ex3: return "RBO_Ex3.exe"
        }
    }
}

/// Clears the 2P/3P keyboard assignments (KeyConfig.cdt/.v2/.v3/.v4), keeping a .bak of the originals.
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
}

/// "화면 크기" choices of the original RBO_Ex3_Config.exe (stored in DATA/SaveData/SystemConfig.v4).
enum ScreenSize: Int, CaseIterable, Identifiable {
    case s640 = 0, psp = 1, s800 = 2, s1024 = 3
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .s640: return "640x480"
        case .psp: return "가상 PSP"
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
