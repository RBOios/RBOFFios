import SwiftUI
import UIKit

/// Landscape only (the game is 1280x1024 and the touch pad is laid out for landscape).
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        return .landscape
    }
}

@main
struct RBOiOSApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            RootView()
        }
    }
}

/// 상단 상태 표시줄(시간·날짜·와이파이·배터리)이 게임 화면을 가리지 않게 숨김.
/// 아이패드는 Info.plist 의 UIStatusBarHidden 만으로는 숨겨지지 않아서 SwiftUI 쪽에서도 지정함.
struct RootView: View {
    @Environment(\.scenePhase) private var scenePhase
    var body: some View {
        content
            .statusBarHidden(true)
            .onChange(of: scenePhase) { phase in
                // 홈 화면·앱 전환으로 앱이 백그라운드로 가면 게임을 멈추고(소리도 같이 멈춤), 돌아오면 이어서 진행.
                // 제어 센터·알림 센터를 내릴 때는 .inactive 만 되므로 멈추지 않음 (눌려 있던 버튼만 풀어 줌).
                switch phase {
                case .active:     rbo_set_paused(0); BGM.resume()
                case .background: PadInput.releaseAll(); rbo_set_paused(1); BGM.suspend()
                default:          PadInput.releaseAll()
                }
            }
    }

    @ViewBuilder private var content: some View {
        if #available(iOS 16.0, *) {
            LauncherView().preferredColorScheme(.dark)
                .persistentSystemOverlays(.hidden)   // 하단 홈 바도 같이 숨김 (살짝 쓸어올리면 나타남)
        } else {
            LauncherView().preferredColorScheme(.dark)
        }
    }
}
