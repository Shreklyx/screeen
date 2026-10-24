import SwiftUI
import WebKit

/// Watching: the website itself, full screen, video playing inline with sound.
struct WatchView: View {
    @AppStorage(Shared.Key.server, store: Shared.defaults) private var server = Shared.defaultServer

    var body: some View {
        if let url = Shared.normalizedServer(server) {
            WebView(url: url)
                .ignoresSafeArea(edges: .bottom)
                .background(Color(red: 0.04, green: 0.04, blue: 0.06))
        } else {
            Text("Adj meg egy érvényes szervercímet a Megosztás fülön.").padding()
        }
    }
}

private struct WebView: UIViewRepresentable {
    let url: URL

    func makeCoordinator() -> Coordinator { Coordinator(origin: url) }

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.allowsInlineMediaPlayback = true
        config.mediaTypesRequiringUserActionForPlayback = []
        config.allowsPictureInPictureMediaPlayback = true
        config.applicationNameForUserAgent = "ScreenShareApp/1.0"
        let view = WKWebView(frame: .zero, configuration: config)
        view.navigationDelegate = context.coordinator
        view.isOpaque = false
        view.backgroundColor = .clear
        view.scrollView.contentInsetAdjustmentBehavior = .automatic
        view.allowsBackForwardNavigationGestures = true
        view.load(URLRequest(url: url))
        return view
    }

    func updateUIView(_ view: WKWebView, context: Context) {
        if context.coordinator.origin.host != url.host {
            context.coordinator.origin = url
            view.load(URLRequest(url: url))
        }
    }

    final class Coordinator: NSObject, WKNavigationDelegate {
        var origin: URL
        init(origin: URL) { self.origin = origin }

        // The server's pages stay inside; any other link opens in Safari.
        @MainActor
        func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction) async -> WKNavigationActionPolicy {
            guard let target = navigationAction.request.url else { return .cancel }
            if target.host == origin.host || target.scheme == "about" || target.scheme == "blob" { return .allow }
            if ["http", "https"].contains(target.scheme ?? "") { _ = await UIApplication.shared.open(target) }
            return .cancel
        }
    }
}
