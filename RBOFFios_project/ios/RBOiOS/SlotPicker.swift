import SwiftUI

/// 퀵 세이브 / 로드 슬롯 선택 화면. 게임의 "캐릭터 슬롯 선택" 패널 모양으로 슬롯 2개를 세로로 보여줍니다.
/// 저장된 슬롯: 캐릭터 그림 + 날짜 / 시간 / 레벨. 비어 있는 슬롯: 빈 패널.
enum SlotMode { case save, load }

/// 슬롯 하나에 대한 정보. 날짜/시간은 상태 파일의 저장 시각, 레벨/직업/성별은 저장할 때 같이 적어 둔 메모(.meta.json)에서 읽습니다.
struct SlotInfo {
    let slot: Int
    let date: Date
    var level: Int?
    var job: Int?
    var sex: Int?
}

enum SlotStore {
    static let count = 2
    static var dir: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    /// 슬롯 1은 예전 단일 저장 파일(rbo_quicksave1.state)을 그대로 씁니다 (이미 저장해 둔 게임이 슬롯 1로 보임).
    static func stateURL(_ slot: Int) -> URL { dir.appendingPathComponent("rbo_quicksave\(slot).state") }
    static func metaURL(_ slot: Int) -> URL { dir.appendingPathComponent("rbo_quicksave\(slot).meta.json") }

    private struct Meta: Codable { var level: Int; var job: Int; var sex: Int }

    static func info(_ slot: Int) -> SlotInfo? {
        var t: Int64 = 0; var f: Int32 = 0
        guard rbo_state_info(stateURL(slot).path, &t, &f) == 0 else { return nil }
        var i = SlotInfo(slot: slot, date: Date(timeIntervalSince1970: TimeInterval(t)), level: nil, job: nil, sex: nil)
        if let d = try? Data(contentsOf: metaURL(slot)), let m = try? JSONDecoder().decode(Meta.self, from: d) {
            i.level = m.level; i.job = m.job; i.sex = m.sex
        }
        return i
    }

    /// 저장 직전에 읽어 둔 캐릭터 정보 (엔진에서 못 읽으면 nil)
    static func currentPlayer() -> (level: Int, job: Int, sex: Int)? {
        var lv: Int32 = 0, jb: Int32 = 0, sx: Int32 = 0
        guard rbo_player_info(&lv, &jb, &sx) == 0 else { return nil }
        return (Int(lv), Int(jb), Int(sx))
    }

    /// 저장에 성공했을 때 부름. 정보가 없으면 예전 메모를 지워서 다른 캐릭터 정보가 남지 않게 함.
    static func writeMeta(_ slot: Int, _ p: (level: Int, job: Int, sex: Int)?) {
        if let p = p, let d = try? JSONEncoder().encode(Meta(level: p.level, job: p.job, sex: p.sex)) { try? d.write(to: metaURL(slot)) }
        else { try? FileManager.default.removeItem(at: metaURL(slot)) }
    }
}

/// 직업 번호(게임의 직업 목록 순서) -> 이름 / 그림.
enum JobInfo {
    static let names = ["노비스", "슈퍼노비스", "소드맨", "나이트", "크루세이더", "아처", "헌터", "바드", "댄서", "매지션", "위자드", "세이지",
                        "머천트", "블랙스미스", "알케미스트", "어콜라이트", "프리스트", "몽크", "씨프", "어쌔신", "로그"]
    static let base = ["NOVICE", "NOVICE", "SWORDMAN", "SWORDMAN", "SWORDMAN", "ARCHER", "ARCHER", "ARCHER", "ARCHER", "MAGICAIN", "MAGICAIN", "MAGICAIN",
                       "MERCHANT", "MERCHANT", "MERCHANT", "ACOLYTE", "ACOLYTE", "ACOLYTE", "THIEF", "THIEF", "THIEF"]
    static func name(_ job: Int) -> String { job >= 0 && job < names.count ? tr(names[job]) : "" }
    /// 성별 값 0 = 여자로 가정 (여자 아처 한 명에서만 확인함). 맞지 않으면 여기만 바꾸면 됨.
    static func portrait(job: Int, sex: Int) -> String? {
        guard job >= 0 && job < base.count else { return nil }
        return "Portrait_\(base[job])_\(sex == 0 ? "F" : "M")"
    }
}

/// 화면 위에 슬롯 선택 창을 띄우고 고른 슬롯을 QuickStateBar에 알려 주는 연결 고리.
final class SlotUI: ObservableObject {
    static let shared = SlotUI()
    @Published var mode: SlotMode? = nil
    var pick: ((SlotMode, Int) -> Void)? = nil
}

/// 게임 패널 모양: 왼쪽이 조금 높고 가운데에서 비스듬히 내려오는 어두운 회색 판.
struct SlotPanelShape: Shape {
    func path(in r: CGRect) -> Path {
        let w = r.width, h = r.height, e: CGFloat = 3          // e: 둥근 모서리를 만들려고 안쪽으로 줄이는 값 (같은 색 테두리를 덧그림)
        var p = Path()
        p.move(to: CGPoint(x: e, y: e))
        p.addLine(to: CGPoint(x: w * 0.40, y: e))
        p.addLine(to: CGPoint(x: w * 0.45, y: h * 0.11))
        p.addLine(to: CGPoint(x: w - e, y: h * 0.11))
        p.addLine(to: CGPoint(x: w - e, y: h - e))
        p.addLine(to: CGPoint(x: e, y: h - e))
        p.closeSubpath()
        return p
    }
}

/// 글자 테두리(검정) + 흰 글씨: 게임 글꼴 느낌.
private struct Outlined: ViewModifier {
    func body(content: Content) -> some View {
        content.foregroundColor(.white)
            .shadow(color: .black, radius: 0, x: 1, y: 0).shadow(color: .black, radius: 0, x: -1, y: 0)
            .shadow(color: .black, radius: 0, x: 0, y: 1).shadow(color: .black, radius: 0, x: 0, y: -1)
    }
}
private extension View { func outlined() -> some View { modifier(Outlined()) } }

struct SlotPanel: View {
    let slot: Int
    let info: SlotInfo?
    let enabled: Bool

    private var dateText: String {
        guard let i = info else { return "" }
        let f = DateFormatter(); f.dateFormat = "yyyy.MM.dd"; return f.string(from: i.date)
    }
    private var timeText: String {
        guard let i = info else { return "" }
        let f = DateFormatter(); f.dateFormat = "HH:mm"; return f.string(from: i.date)
    }

    var body: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            let fs = h * 0.115
            ZStack(alignment: .topLeading) {
                SlotPanelShape().fill(Color(white: 0.31).opacity(0.96))
                SlotPanelShape().stroke(Color(white: 0.31).opacity(0.96), style: StrokeStyle(lineWidth: 6, lineJoin: .round))
                if let i = info {
                    if let job = i.job, let name = JobInfo.portrait(job: job, sex: i.sex ?? 0) {
                        Image(name).resizable().scaledToFit()
                            .frame(width: w * 0.34, height: w * 0.34)
                            .frame(width: w * 0.34, height: h, alignment: .top).clipped()      // 위쪽(얼굴~상체)만 보이게 자름
                            .position(x: w * 0.02 + w * 0.17, y: h / 2)
                        Text(JobInfo.name(job)).font(.system(size: fs, weight: .heavy)).outlined()
                            .position(x: w * 0.02 + fs * 1.6, y: h * 0.90)
                    }
                    Group {
                        Text(tr("날짜")).position(x: w * 0.50, y: h * 0.30)
                        Text(tr("시간")).position(x: w * 0.50, y: h * 0.49)
                        Text(tr("레벨")).position(x: w * 0.50, y: h * 0.68)
                    }.font(.system(size: fs, weight: .heavy)).outlined()
                    Group {
                        Text(dateText).position(x: w * 0.69, y: h * 0.30)
                        Text(timeText).position(x: w * 0.69, y: h * 0.49)
                        Text(i.level.map { String($0) } ?? "-").position(x: w * 0.69, y: h * 0.68)
                    }.font(.system(size: fs, weight: .heavy, design: .monospaced)).outlined()
                    Text(trf("슬롯 %d", slot)).font(.system(size: fs * 0.95, weight: .heavy)).outlined()
                        .position(x: w * 0.94, y: h * 0.24)
                }
            }
        }
        .aspectRatio(632.0 / 147.0, contentMode: .fit)
        .opacity(enabled ? 1 : 0.55)
        .contentShape(Rectangle())
    }
}

struct SlotPickerView: View {
    let mode: SlotMode
    @ObservedObject private var ui = SlotUI.shared
    @State private var infos: [SlotInfo?] = []

    var body: some View {
        ZStack {
            Color.black.opacity(0.55).ignoresSafeArea().onTapGesture { ui.mode = nil }
            GeometryReader { geo in
                let pw = min(geo.size.width * 0.72, 600, (geo.size.height - 32) / CGFloat(SlotStore.count) * 632.0 / 147.0)
                VStack(spacing: 8) {
                    ForEach(0..<SlotStore.count, id: \.self) { n in
                        let info: SlotInfo? = n < infos.count ? infos[n] : nil
                        SlotPanel(slot: n + 1, info: info, enabled: mode == .save || info != nil)
                            .frame(width: pw)
                            .onTapGesture {
                                if mode == .load && info == nil { return }          // 비어 있는 슬롯은 불러올 수 없음
                                ui.pick?(mode, n + 1)
                            }
                    }
                }
                .frame(width: geo.size.width, height: geo.size.height)
            }
            VStack {
                HStack {
                    Text(mode == .save ? tr("저장할 슬롯") : tr("불러올 슬롯")).font(.system(size: 16, weight: .heavy)).outlined()
                    Spacer()
                    Button(action: { ui.mode = nil }) {
                        Image(systemName: "xmark").font(.system(size: 18, weight: .bold)).foregroundColor(.white)
                            .frame(width: 44, height: 44).contentShape(Rectangle())
                    }
                }
                Spacer()
            }.padding(.horizontal, 20).padding(.top, 6)
        }
        .onAppear { infos = (1...SlotStore.count).map { SlotStore.info($0) } }
    }
}
