// Plain checks for the parts that do not need a device: `swiftc Broadcast/Sdp.swift Shared/Shared.swift Tests/main.swift`
import Foundation

var failures = 0
func check(_ ok: Bool, _ message: String) {
    print(ok ? "PASS: \(message)" : "FAIL: \(message)")
    if !ok { failures += 1 }
}

// an answer as Chrome sends it
let answer = [
    "v=0", "o=- 1 2 IN IP4 127.0.0.1", "s=-", "t=0 0", "a=group:BUNDLE 0 1",
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98",
    "a=mid:0",
    "a=rtpmap:96 H264/90000",
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
    "a=rtpmap:97 rtx/90000",
    "a=fmtp:97 apt=96",
    "a=rtpmap:98 VP8/90000",
    "m=audio 9 UDP/TLS/RTP/SAVPF 111",
    "a=mid:1",
    "a=rtpmap:111 opus/48000/2",
    "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1",
    "",
].joined(separator: "\r\n")
let extra = "x-google-start-bitrate=3000;x-google-max-bitrate=20000"
let tuned = Sdp.tuneAnswer(answer, startKbps: 3000, maxKbps: 20000)
let lines = tuned.components(separatedBy: "\r\n")
check(lines.contains("a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f;\(extra)"), "H.264 fmtp extended")
if let rtpmap = lines.firstIndex(of: "a=rtpmap:98 VP8/90000") {
    check(lines[rtpmap + 1] == "a=fmtp:98 \(extra)", "VP8 gets an fmtp line right after its rtpmap")
} else {
    check(false, "VP8 rtpmap kept")
}
check(lines.contains("a=fmtp:97 apt=96"), "RTX untouched")
check(lines.contains("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1"), "audio untouched")
check(tuned.hasSuffix("\r\n"), "trailing CRLF kept")
check(Sdp.tuneAnswer(tuned, startKbps: 3000, maxKbps: 20000) == tuned, "tuning twice changes nothing")

check(Shared.normalizedServer("mozizas.hu")?.absoluteString == "https://mozizas.hu/", "bare host → https")
check(Shared.normalizedServer(" https://mozizas.hu/x?y#abc123 ")?.absoluteString == "https://mozizas.hu/", "path, query and code dropped")
check(Shared.normalizedServer("http://192.168.1.10:8080")?.absoluteString == "http://192.168.1.10:8080/", "local http with port")
check(Shared.normalizedServer("ftp://x") == nil && Shared.normalizedServer("") == nil, "invalid rejected")

var status = ShareStatus(state: .live, code: "abc123", link: "https://mozizas.hu/#abc123", viewers: 2)
status.save()
check(ShareStatus.load().code == "abc123" && ShareStatus.load().viewers == 2, "status round trip")

exit(failures == 0 ? 0 : 1)
