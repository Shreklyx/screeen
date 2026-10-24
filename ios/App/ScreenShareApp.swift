import SwiftUI

@main
struct ScreenShareApp: App {
    var body: some Scene {
        WindowGroup {
            TabView {
                ShareView()
                    .tabItem { Label("Megosztás", systemImage: "rectangle.inset.filled.and.person.filled") }
                WatchView()
                    .tabItem { Label("Nézés", systemImage: "play.rectangle.fill") }
            }
            .tint(Color(red: 1, green: 0.30, blue: 0.37))
            .preferredColorScheme(.dark)
        }
    }
}
