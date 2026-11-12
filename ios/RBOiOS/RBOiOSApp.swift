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
            LauncherView().preferredColorScheme(.dark)
        }
    }
}
