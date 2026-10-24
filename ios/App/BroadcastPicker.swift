import ReplayKit
import SwiftUI
import UIKit

/// iOS starts a screen broadcast only from its own picker. The picker view lives
/// (invisibly) in the screen, and our button presses its internal button.
final class BroadcastPicker {
    static let shared = BroadcastPicker()

    let view: RPSystemBroadcastPickerView = {
        let picker = RPSystemBroadcastPickerView(frame: CGRect(x: 0, y: 0, width: 44, height: 44))
        picker.preferredExtension = BroadcastPicker.extensionId
        picker.showsMicrophoneButton = false
        return picker
    }()

    /// Bundle id of our broadcast extension, read from the app (sideloading tools may rename ids).
    static var extensionId: String? {
        guard let plugins = Bundle.main.builtInPlugInsURL,
              let items = try? FileManager.default.contentsOfDirectory(at: plugins, includingPropertiesForKeys: nil)
        else { return nil }
        return items.lazy
            .filter { $0.pathExtension == "appex" }
            .compactMap { Bundle(url: $0)?.bundleIdentifier }
            .first
    }

    func show() {
        for case let button as UIButton in view.subviews {
            button.sendActions(for: .allTouchEvents)
            return
        }
    }
}

struct BroadcastPickerHost: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView {
        let container = UIView()
        container.addSubview(BroadcastPicker.shared.view)
        container.isUserInteractionEnabled = false
        return container
    }
    func updateUIView(_ uiView: UIView, context: Context) {}
}
