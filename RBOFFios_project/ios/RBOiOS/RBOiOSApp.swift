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
    var body: some View {
        content
            .statusBarHidden(true)
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
