import Foundation

/// The four executables that ship with the game folder.
enum Edition: String, CaseIterable, Identifiable {
    case original, ex1, ex2, ex3
    var id: String { rawValue }
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
