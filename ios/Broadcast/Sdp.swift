import Foundation

enum Sdp {
    private static let videoCodecs: Set<String> = ["H264", "H265", "VP8", "VP9", "AV1"]

    /// Same tuning the website does for the viewer's answer: high start bitrate and
    /// libwebrtc's 2.5 Mbit/s default ceiling lifted (the real cap is on the sender).
    static func tuneAnswer(_ sdp: String, startKbps: Int, maxKbps: Int) -> String {
        let extra = "x-google-start-bitrate=\(startKbps);x-google-max-bitrate=\(maxKbps)"
        let lines = sdp.components(separatedBy: "\r\n")

        // payload types of the video codecs, and which of them already have an fmtp line
        var kind = ""
        var videoTypes: [String] = []
        var withFmtp = Set<String>()
        for line in lines {
            if line.hasPrefix("m=") {
                kind = String(line.dropFirst(2).prefix { $0 != " " })
            } else if kind == "video", let rtpmap = attribute(line, "a=rtpmap:") {
                let name = rtpmap.value.split(separator: "/").first.map { $0.uppercased() } ?? ""
                if videoCodecs.contains(name) { videoTypes.append(rtpmap.pt) }
            } else if kind == "video", let fmtp = attribute(line, "a=fmtp:") {
                withFmtp.insert(fmtp.pt)
            }
        }

        var out: [String] = []
        kind = ""
        for line in lines {
            if line.hasPrefix("m=") { kind = String(line.dropFirst(2).prefix { $0 != " " }) }
            if kind == "video", let fmtp = attribute(line, "a=fmtp:"), videoTypes.contains(fmtp.pt), !line.contains("x-google-max-bitrate") {
                out.append("\(line);\(extra)")
                continue
            }
            out.append(line)
            if kind == "video", let rtpmap = attribute(line, "a=rtpmap:"), videoTypes.contains(rtpmap.pt), !withFmtp.contains(rtpmap.pt) {
                out.append("a=fmtp:\(rtpmap.pt) \(extra)")
            }
        }
        return out.joined(separator: "\r\n")
    }

    /// "a=rtpmap:96 H264/90000" → (pt: "96", value: "H264/90000")
    static func attribute(_ line: String, _ prefix: String) -> (pt: String, value: String)? {
        guard line.hasPrefix(prefix) else { return nil }
        let body = line.dropFirst(prefix.count)
        guard let space = body.firstIndex(of: " ") else { return nil }
        return (String(body[..<space]), String(body[body.index(after: space)...]))
    }
}
