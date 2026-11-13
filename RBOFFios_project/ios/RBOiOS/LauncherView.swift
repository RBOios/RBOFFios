import SwiftUI

struct LauncherView: View {
    @AppStorage("gameFolderBookmark") private var bookmark: Data = Data()
    @State private var folderName: String = ""
    @State private var showPicker = false
    @State private var running: Edition? = nil
    @State private var gameURL: URL? = nil
    @State private var message: String = ""
    @AppStorage("screenSizeMode") private var savedMode: Int = -1      // -1 = never chosen: leave the game's own setting alone
    @State private var screenMode: Int = 0
    private var modeBinding: Binding<Int> { Binding(get: { screenMode }, set: { screenMode = $0; savedMode = $0 }) }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if running == nil {
                GeometryReader { geo in
                    Image("Background")
                        .resizable()
                        .scaledToFill()
                        .frame(width: geo.size.width, height: geo.size.height)
                        .clipped()
                }.ignoresSafeArea()
            }
            if let ed = running, let url = gameURL {
                GameScreen(edition: ed, root: url)
            } else {
                menu
            }
        }
        .fileImporter(isPresented: $showPicker, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result { saveBookmark(url) }
        }
        .onAppear { restoreName() }
    }

    private var menu: some View {
        VStack(alignment: .leading, spacing: 18) {
            ForEach(Edition.available) { ed in
                Button(action: { launch(ed) }) {
                    HStack { Text("\(ed.title) 시작하기").font(.system(size: 26, weight: .bold)); Text("▶").font(.system(size: 22)) }
                        .foregroundColor(.white)
                }
            }
            Divider().background(Color.gray)
            Menu {
                Picker("화면 크기", selection: modeBinding) {
                    ForEach(ScreenSize.allCases) { Text($0.title).tag($0.rawValue) }
                }
            } label: {
                Text("화면 크기: \(ScreenSize(rawValue: screenMode)?.title ?? "640x480") ▾").foregroundColor(.cyan)
            }
            Button("게임 폴더 선택…") { showPicker = true }.foregroundColor(.cyan)
            Text(folderName.isEmpty ? "게임 폴더가 선택되지 않았어요 (RBO.exe와 DATA 폴더가 있는 곳)" : "폴더: \(folderName)")
                .font(.footnote).foregroundColor(.gray)
            if !message.isEmpty { Text(message).font(.footnote).foregroundColor(.orange) }
        }
        .fixedSize(horizontal: true, vertical: false)   // panel hugs its content (Divider would otherwise stretch it full width)
        .padding(24)
        .background(Color.black.opacity(0.6))
        .cornerRadius(16)
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
    }

    private func saveBookmark(_ url: URL) {
        guard url.startAccessingSecurityScopedResource() else { message = "폴더 접근 권한을 얻지 못했어요."; return }
        defer { url.stopAccessingSecurityScopedResource() }
        if let data = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
            bookmark = data; folderName = url.lastPathComponent; message = ""
        }
    }
    private func restoreName() {
        var stale = false
        if !bookmark.isEmpty, let u = try? URL(resolvingBookmarkData: bookmark, options: [], relativeTo: nil, bookmarkDataIsStale: &stale) {
            folderName = u.lastPathComponent
            if savedMode >= 0 { screenMode = savedMode }
            else if u.startAccessingSecurityScopedResource() {                 // show what the game folder is currently set to
                if let cur = SystemConfigPatcher.current(root: u) { screenMode = cur.rawValue }
                u.stopAccessingSecurityScopedResource()
            }
        } else if savedMode >= 0 { screenMode = savedMode }
    }
    private func launch(_ ed: Edition) {
        var stale = false
        guard !bookmark.isEmpty,
              let url = try? URL(resolvingBookmarkData: bookmark, options: [], relativeTo: nil, bookmarkDataIsStale: &stale),
              url.startAccessingSecurityScopedResource() else { message = "먼저 게임 폴더를 선택해 주세요."; return }
        let exe = url.appendingPathComponent(ed.exeName)
        guard FileManager.default.fileExists(atPath: exe.path) else {
            message = "\(ed.exeName) 파일을 찾지 못했어요."; url.stopAccessingSecurityScopedResource(); return
        }
        KeyConfigPatcher.apply(root: url)
        if savedMode >= 0, let size = ScreenSize(rawValue: savedMode) { SystemConfigPatcher.apply(root: url, size: size) }
        message = ""; gameURL = url; running = ed        // access stays open while the game runs
    }
}
