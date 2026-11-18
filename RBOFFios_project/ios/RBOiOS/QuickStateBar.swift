import SwiftUI

/// 화면 왼쪽 위(음량 아이콘 오른쪽)의 "저장 / 불러오기" 버튼 (슬롯 1개, 세로로 저장 → 불러오기, 흰색 아이콘).
/// 엔진이 게임 전체 상태(메모리·CPU·사운드·화면)를 파일 하나로 저장했다가 그대로 되돌립니다.
/// 파일: Files 앱 > RBO 폴더의 rbo_quicksave1.state
struct QuickStateBar: View {
    @State private var message: String? = nil
    @State private var working = false
    @State private var askLoad = false
    @State private var loadInfo = ""
    @State private var lastOp: Int32 = 0          // 1 = save, 2 = load: tells which table the result code belongs to
    private let poll = Timer.publish(every: 0.2, on: .main, in: .common).autoconnect()

    private static var fileURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("rbo_quicksave1.state")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            VStack(alignment: .leading, spacing: 6) {   // 저장 아래에 불러오기
                iconButton("SaveIcon") { start(1) }
                iconButton("LoadIcon") { prepareLoad() }
            }
            if let m = message {
                Text(m).font(.system(size: 13, weight: .semibold)).foregroundColor(Color.white)
                    .padding(.horizontal, 10).padding(.vertical, 5)
                    .background(Color.black.opacity(0.6)).cornerRadius(8)
                    .allowsHitTesting(false)
            }
        }
        .alert(tr("저장한 게임을 불러올까요?"), isPresented: $askLoad) {
            Button(tr("취소"), role: .cancel) {}
            Button(tr("불러오기"), role: .destructive) { start(2) }
        } message: {
            Text(loadInfo)
        }
        .onReceive(poll) { _ in check() }
    }

    /// 흰색 아이콘 버튼 (음량 아이콘과 같은 톤). 아이콘은 Assets의 SaveIcon / LoadIcon (22pt = 66px @3x).
    private func iconButton(_ name: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(name)
                .resizable().interpolation(.none)
                .frame(width: 22, height: 22)
                .opacity(working ? 0.25 : 0.55)
                .frame(width: 44, height: 40).contentShape(Rectangle())
        }
        .disabled(working)
    }

    private func chip(_ title: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 13, weight: .bold))
                .foregroundColor(Color.white.opacity(0.55))
                .padding(.horizontal, 12).frame(minHeight: 34)
                .background(Color.black.opacity(0.28))
                .overlay(Capsule().stroke(Color.white.opacity(0.32), lineWidth: 1.2))
                .clipShape(Capsule())
        }
        .disabled(working)
    }

    private func show(_ text: String) {
        message = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { if message == text { message = nil } }
    }

    private func start(_ op: Int32) {
        if rbo_state_request(op, Self.fileURL.path) == 0 { working = true; lastOp = op; message = op == 1 ? tr("저장 중…") : tr("불러오는 중…") }
        else { show(tr("잠시 후 다시 시도해 주세요")) }
    }

    private func prepareLoad() {
        var t: Int64 = 0; var f: Int32 = 0
        guard rbo_state_info(Self.fileURL.path, &t, &f) == 0 else { show(tr("저장된 게임이 없어요")); return }
        let fmt = DateFormatter()
        if AppLang.current == .ko { fmt.locale = Locale(identifier: "ko_KR"); fmt.dateFormat = "M월 d일 a h:mm" }
        else { fmt.locale = Locale(identifier: "en_US"); fmt.dateFormat = "MMM d, h:mm a" }
        loadInfo = trf("%@에 저장한 상태예요.\n지금 진행 중인 내용은 사라져요.", fmt.string(from: Date(timeIntervalSince1970: TimeInterval(t))))
        askLoad = true
    }

    private func check() {
        guard working else { return }
        let r = rbo_state_result()
        if r == 1 { return }
        working = false
        show(Self.resultText(op: lastOp, code: r))
    }

    /// Localized text for the engine's result code (the engine itself only has Korean text, used here as a fallback).
    private static func resultText(op: Int32, code: Int32) -> String {
        let save: [Int32: String] = [2: "저장했어요", -1: "지금은 저장할 수 없어요", -2: "영상 재생 중에는 저장할 수 없어요", -3: "파일을 만들 수 없어요",
                                     -4: "저장 중 오류 (저장 공간이 부족할 수 있어요)", -5: "저장 파일을 바꿀 수 없어요"]
        let load: [Int32: String] = [3: "불러왔어요", -1: "지금은 불러올 수 없어요", -2: "저장된 게임이 없어요", -3: "저장 파일 형식이 맞지 않아요", -4: "저장 파일이 손상되었어요",
                                     -5: "다른 실행 파일의 저장이에요", -6: "이 버전과 호환되지 않는 저장이에요", -7: "파일을 열 수 없어요", -8: "불러오기 실패 (게임을 다시 시작해 주세요)"]
        if let ko = (op == 1 ? save : load)[code] { return tr(ko) }
        return String(cString: rbo_state_message())
    }
}
