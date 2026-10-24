import Foundation

/// What the app and the broadcast extension share: the settings (server, password)
/// and the state of a running share (room code, link, viewers).
enum Shared {
    static let defaultServer = "https://mozizas.hu/"
    static let defaultGroup = "group.hu.mozizas.screenshare"

    enum Key {
        static let server = "server"
        static let password = "password"
        static let shareAudio = "shareAudio"
        static let status = "status"
    }

    /// The App Group this build was really signed with. Sideloading tools (Sideloadly,
    /// AltStore) rename it per Apple ID, so it is read from the embedded provisioning
    /// profile; the name in the project is only the fallback.
    static let appGroup: String = {
        guard let url = Bundle.main.url(forResource: "embedded", withExtension: "mobileprovision"),
              let data = try? Data(contentsOf: url),
              let start = data.range(of: Data("<?xml".utf8)),
              let end = data.range(of: Data("</plist>".utf8), options: [], in: start.lowerBound..<data.endIndex)
        else { return defaultGroup }
        let plist = data.subdata(in: start.lowerBound..<end.upperBound)
        guard let profile = try? PropertyListSerialization.propertyList(from: plist, format: nil) as? [String: Any],
              let entitlements = profile["Entitlements"] as? [String: Any],
              let groups = entitlements["com.apple.security.application-groups"] as? [String],
              let group = groups.first(where: { !$0.contains("*") })
        else { return defaultGroup }
        return group
    }()

    static let defaults: UserDefaults = UserDefaults(suiteName: appGroup) ?? .standard

    static var server: String { defaults.string(forKey: Key.server) ?? defaultServer }
    static var password: String { defaults.string(forKey: Key.password) ?? "" }
    static var shareAudio: Bool { defaults.object(forKey: Key.shareAudio) as? Bool ?? true }

    /// "mozizas.hu", "https://mozizas.hu/x" → https://mozizas.hu/
    static func normalizedServer(_ raw: String) -> URL? {
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, var parts = URLComponents(string: text.contains("://") ? text : "https://\(text)"),
              let scheme = parts.scheme?.lowercased(), scheme == "https" || scheme == "http", parts.host?.isEmpty == false
        else { return nil }
        parts.path = "/"
        parts.query = nil
        parts.fragment = nil
        return parts.url
    }
}

/// State of the share, written by the extension, shown by the app.
struct ShareStatus: Codable, Equatable {
    enum State: String, Codable { case idle, connecting, live, paused, failed }

    var state: State = .idle
    var code: String?
    var link: String?
    var viewers = 0
    var message: String?
    var updated = Date()

    static func load() -> ShareStatus {
        guard let data = Shared.defaults.data(forKey: Shared.Key.status),
              let status = try? JSONDecoder().decode(ShareStatus.self, from: data)
        else { return ShareStatus() }
        return status
    }

    func save() {
        var copy = self
        copy.updated = Date()
        if let data = try? JSONEncoder().encode(copy) { Shared.defaults.set(data, forKey: Shared.Key.status) }
    }
}
