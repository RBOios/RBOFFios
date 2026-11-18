import SwiftUI

struct LauncherView: View {
    @AppStorage("gameFolderBookmark") private var bookmark: Data = Data()
    @State private var folderName: String = ""
    @State private var showPicker = false
    @State private var showControls = false    // 컨트롤 설정 화면
    @State private var running: Edition? = nil
    @State private var gameURL: URL? = nil
    @State private var message: String = ""
    @AppStorage("screenSize") private var screenSizeRaw: Int = ScreenSize.s640.rawValue
    @AppStorage(AppLang.key) private var langRaw: String = ""    // "" = not chosen yet (AppLang.defaultLang is used); changing it redraws this view
    private var screenSize: ScreenSize { ScreenSize(rawValue: screenSizeRaw) ?? .s640 }
    /// Title background: one of TITLE_BG001...008, picked at random each time the app is launched (a static is evaluated once per launch,
    /// so it stays the same while the launcher is shown again after the game ends).
    private static let titleBackground = "TITLE_BG00\(Int.random(in: 1...8))"
    private let panelContentWidth: CGFloat = 240   // 카드 안쪽 최소 폭. 영어 화면 기준 폭이라 한국어도 같은 크기로 보임 (카드 전체 = 240 + 좌우 패딩 24x2)
    private var lang: AppLang { AppLang(rawValue: langRaw) ?? AppLang.defaultLang }   // langRaw를 body에서 읽어야 SwiftUI가 언어가 바뀔 때 화면을 다시 그림

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if running == nil {
                GeometryReader { geo in
                    Image(Self.titleBackground)
                        .resizable()
                        .scaledToFill()
                        .frame(width: geo.size.width, height: geo.size.height)
                        .clipped()
                }.ignoresSafeArea()
            }
            if let ed = running, let url = gameURL {
                GameScreen(edition: ed, root: url, screenSize: screenSize)
            } else {
                menu.id(langRaw)    // 언어가 바뀌면 메뉴를 새로 만들어서 모든 글자가 새 언어로 바뀌게 함
            }
        }
        .fileImporter(isPresented: $showPicker, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result { saveBookmark(url) }
        }
        .fullScreenCover(isPresented: $showControls) { ControlsView() }
        .onAppear { restoreName(); if running == nil { BGM.play() } }
        .onChange(of: running) { r in if r == nil { BGM.play() } else { BGM.stop() } }
    }

    private var menu: some View {
        VStack(alignment: .leading, spacing: 18) {
            ForEach(Edition.available) { ed in
                Button(action: { ClickSound.play(); launch(ed) }) {
                    HStack { Text(trf("%@ 시작하기", ed.title)).font(.system(size: 26, weight: .bold)); Text("▶").font(.system(size: 22)) }
                        .foregroundColor(.white)
                }
            }
            Divider().background(Color.gray)
            HStack {
                Text(tr("화면 크기")).foregroundColor(.white)
                Picker(tr("화면 크기"), selection: $screenSizeRaw) {
                    ForEach(ScreenSize.allCases) { Text($0.title).tag($0.rawValue) }
                }
                .pickerStyle(.menu).accentColor(.cyan).lineLimit(1).fixedSize()   // "640x480"이 두 줄로 꺾이지 않게
            }
            Button(tr("컨트롤 설정")) { ClickSound.play(); showControls = true }.foregroundColor(.cyan)
            Button(tr("게임 폴더 선택…")) { ClickSound.play(); showPicker = true }.foregroundColor(.cyan)
            Text(folderName.isEmpty ? tr("게임 폴더가 선택되지 않았어요 (RBO_Ex3.exe와 DATA 폴더가 있는 곳)") : trf("폴더: %@", folderName))
                .font(.footnote).foregroundColor(.gray)
                .frame(maxWidth: panelContentWidth, alignment: .leading).fixedSize(horizontal: false, vertical: true)   // 긴 안내문은 카드를 늘리지 않고 줄바꿈
            if !message.isEmpty {
                Text(message).font(.footnote).foregroundColor(.orange)
                    .frame(maxWidth: panelContentWidth, alignment: .leading).fixedSize(horizontal: false, vertical: true)
            }
            HStack(spacing: 10) {                                   // language switch: 한국어 | English
                Text("🌐").font(.system(size: 14))
                ForEach(AppLang.allCases) { l in
                    Button(action: { ClickSound.play(); langRaw = l.rawValue; message = "" }) {
                        Text(l.label).font(.system(size: 14, weight: lang == l ? .bold : .regular))
                            .foregroundColor(lang == l ? .white : .gray)
                    }
                    if l != AppLang.allCases.last { Text("|").font(.system(size: 14)).foregroundColor(.gray) }
                }
            }
        }
        .frame(minWidth: panelContentWidth, alignment: .leading)   // 언어가 바뀌어도 카드 폭이 줄지 않게 (영어 기준 폭)
        .fixedSize(horizontal: true, vertical: false)   // panel hugs its content (Divider would otherwise stretch it full width)
        .padding(24)
        .background(Color.black.opacity(0.6))
        .cornerRadius(16)
        .padding(32)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
    }

    private func saveBookmark(_ url: URL) {
        guard url.startAccessingSecurityScopedResource() else { message = tr("폴더 접근 권한을 얻지 못했어요."); return }
        defer { url.stopAccessingSecurityScopedResource() }
        if let data = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
            bookmark = data; folderName = url.lastPathComponent; message = ""
        }
    }
    private func restoreName() {
        var stale = false
        if !bookmark.isEmpty, let u = try? URL(resolvingBookmarkData: bookmark, options: [], relativeTo: nil, bookmarkDataIsStale: &stale) {
            folderName = u.lastPathComponent
        }
    }
    private func launch(_ ed: Edition) {
        var stale = false
        guard !bookmark.isEmpty,
              let url = try? URL(resolvingBookmarkData: bookmark, options: [], relativeTo: nil, bookmarkDataIsStale: &stale),
              url.startAccessingSecurityScopedResource() else { message = tr("먼저 게임 폴더를 선택해 주세요."); return }
        let exe = url.appendingPathComponent(ed.exeName)
        guard FileManager.default.fileExists(atPath: exe.path) else {
            message = trf("%@ 파일을 찾지 못했어요.", ed.exeName); url.stopAccessingSecurityScopedResource(); return
        }
        KeyConfigPatcher.apply(root: url)
        KeyConfigPatcher.applyPlayerOne(root: url, controls: GameControls.shared)   // 컨트롤 설정의 게임 키를 게임의 KeyConfig.v4에 기록
        SystemConfigPatcher.apply(root: url, size: screenSize)  // the launcher's screen-size choice; the picture is always 640x480 and the GPU scales it
        message = ""; gameURL = url; running = ed        // access stays open while the game runs
    }
}
