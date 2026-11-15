import SwiftUI

/// BGM 음량 조절: 화면 왼쪽 위의 스피커 아이콘을 누르면 BGM 슬라이더가 열립니다 (효과음·동영상 소리는 그대로).
/// 값은 앱에 저장되어 다음 실행에도 유지됩니다. 블루투스 패드가 연결되면 GameScreen이 이 아이콘을 숨깁니다(터치 패드와 같은 규칙).
struct VolumeControl: View {
    @AppStorage("rboVolBgm") private var bgm = 0.5      // BGM이 효과음보다 큰 편이라 기본값을 절반으로
    @State private var open = false

    private func apply() { rbo_set_volume(1.0, Float(bgm)) }   // 첫 값 = 전체 음량 (항상 100%), 둘째 값 = BGM

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Button { open.toggle() } label: {
                Image(systemName: bgm <= 0.001 ? "speaker.slash.fill" : "speaker.wave.2.fill")
                    .font(.system(size: 17, weight: .bold))
                    .foregroundColor(Color.white.opacity(open ? 0.85 : 0.40))
                    .frame(width: 44, height: 38).contentShape(Rectangle())
            }
            if open {
                HStack(spacing: 8) {
                    Text("BGM").font(.system(size: 13, weight: .semibold)).foregroundColor(Color.white.opacity(0.9)).frame(width: 36, alignment: .leading)
                    Slider(value: $bgm, in: 0...1)
                    Text("\(Int((bgm * 100).rounded()))%")
                        .font(.system(size: 12, design: .monospaced)).foregroundColor(Color.white.opacity(0.8)).frame(width: 42, alignment: .trailing)
                }
                .padding(.horizontal, 10).padding(.vertical, 8)
                .frame(width: 260)
                .background(Color.black.opacity(0.62))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.white.opacity(0.28), lineWidth: 1))
                .clipShape(RoundedRectangle(cornerRadius: 10))
            }
        }
        .onAppear { apply() }
        .onChange(of: bgm) { _ in apply() }
    }
}
