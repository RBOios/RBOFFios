import SwiftUI

/// PC 트레이너(rbo_ex3_trainer)의 기능: HP/SP/스킬 포인트/스탯 포인트/화살/돈.
/// 고정 = 지금 값을 계속 유지, 채우기 = 그 값으로 설정(HP·SP), + = 현재 값에 더하기(나머지). ×N 버튼으로 양을 10배씩 바꿈.
/// 확장팩 3(RBO_Ex3.exe)에서만 동작하고, 다른 실행 파일에서는 값이 "—"로 보이며 아무 일도 일어나지 않습니다.
struct CheatButton: View {
    let open: Bool
    var body: some View {
        // PAUSE 버튼과 같은 모양(사각 테두리 + 글자, 투명 배경). 크기는 PAUSE 그림과 같은 비율의 작은 버전.
        ZStack {
            Rectangle().stroke(Color.white.opacity(open ? 0.80 : 0.32), lineWidth: 2)
            GeometryReader { g in
                Text("치트").font(.system(size: g.size.height * 0.50, weight: .bold))
                    .foregroundColor(Color.white.opacity(open ? 0.80 : 0.32))
                    .frame(width: g.size.width, height: g.size.height)
            }
        }
        .aspectRatio(1264.0 / 496.0, contentMode: .fit)
        .padding(10)                       // PAUSE처럼 그림보다 넓은 터치 영역
        .contentShape(Rectangle())
    }
}

struct CheatPanel: View {
    let close: () -> Void
    private struct Item { let name: String; let base: Int; let setsValue: Bool }
    private let items: [Item] = [
        Item(name: "HP", base: 1000, setsValue: true),
        Item(name: "SP", base: 100, setsValue: true),
        Item(name: "스킬 P", base: 5, setsValue: false),
        Item(name: "스탯 P", base: 10, setsValue: false),
        Item(name: "화살", base: 10, setsValue: false),
        Item(name: "돈", base: 10, setsValue: false),
    ]
    private let mults = [1, 10, 100, 1000]
    @State private var values: [Int32] = Array(repeating: -1, count: 6)
    @State private var mask: Int32 = 0
    @State private var multIdx: [Int] = Array(repeating: 0, count: 6)
    private let poll = Timer.publish(every: 0.4, on: .main, in: .common).autoconnect()

    static let width: CGFloat = 344
    static let height: CGFloat = 36 + 6 * 36 + 14

    var body: some View {
        VStack(spacing: 4) {
            HStack {
                Text("치트").font(.system(size: 14, weight: .bold)).foregroundColor(Color.white.opacity(0.9))
                Spacer()
                Button(action: close) {
                    Text("✕").font(.system(size: 15, weight: .bold)).foregroundColor(Color.white.opacity(0.8))
                        .frame(width: 34, height: 30).contentShape(Rectangle())
                }
            }
            .frame(height: 30)
            ForEach(0..<6, id: \.self) { i in row(i) }
        }
        .padding(.horizontal, 10).padding(.vertical, 6)
        .frame(width: Self.width, height: Self.height)
        .background(Color.black.opacity(0.62))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.white.opacity(0.28), lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: 10))
        .onAppear { refresh() }
        .onReceive(poll) { _ in refresh() }
    }

    private func amount(_ i: Int) -> Int { items[i].base * mults[multIdx[i]] }

    private func row(_ i: Int) -> some View {
        let frozen = (mask >> Int32(i)) & 1 == 1
        return HStack(spacing: 5) {
            Text(items[i].name).font(.system(size: 13, weight: .semibold)).foregroundColor(Color.white.opacity(0.85))
                .frame(width: 46, alignment: .leading)
            Text(values[i] < 0 ? "—" : "\(values[i])").font(.system(size: 13, design: .monospaced)).foregroundColor(Color.white.opacity(0.85))
                .frame(width: 74, alignment: .trailing)
            chip(frozen ? "고정중" : "고정", on: frozen, width: 52) {
                rbo_cheat_freeze(Int32(i), frozen ? 0 : 1)
                mask ^= Int32(1) << Int32(i)
            }
            chip(items[i].setsValue ? "채우기 \(amount(i))" : "+\(amount(i))", on: false, width: 84) {
                if items[i].setsValue { rbo_cheat_set(Int32(i), Int32(amount(i))) } else { rbo_cheat_add(Int32(i), Int32(amount(i))) }
            }
            chip("×\(mults[multIdx[i]])", on: false, width: 46) { multIdx[i] = (multIdx[i] + 1) % mults.count }
        }
        .frame(height: 32)
    }

    private func chip(_ title: String, on: Bool, width: CGFloat, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title).font(.system(size: 12, weight: .bold)).lineLimit(1).minimumScaleFactor(0.6)
                .foregroundColor(on ? Color.black : Color.white.opacity(0.9))
                .frame(width: width, height: 28)
                .background(on ? Color.white.opacity(0.85) : Color.white.opacity(0.14))
                .clipShape(Capsule())
        }
    }

    private func refresh() {
        for i in 0..<6 { values[i] = Int32(rbo_cheat_get(Int32(i))) }
        mask = Int32(rbo_cheat_frozen_mask())
    }
}
