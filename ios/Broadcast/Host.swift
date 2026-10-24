import CoreMedia
import Foundation
import ImageIO
import QuartzCore
import ReplayKit
import WebRTC

/// The sharing side of the Screen Share protocol – what the website's host page does –
/// running inside the broadcast extension: one RTCPeerConnection per viewer, the host
/// always makes the offers, the viewers answer (see public/app.js).
final class Host: NSObject {
    private static let maxVideoBps = 8_000_000
    private static let startKbps = 3_000
    private static let maxKbps = 20_000
    private static let fps: Int32 = 30

    private final class Viewer {
        let id: String
        var session = ""
        var pc: RTCPeerConnection?
        var createdAt = Date()
        var pendingCandidates: [RTCIceCandidate] = []
        var negotiating = false
        var reconnect: DispatchWorkItem?
        init(id: String) { self.id = id }
    }

    private let server: URL
    private let password: String
    private let onFatal: (String) -> Void
    private let queue = DispatchQueue(label: "screenshare.host")

    private let factory: RTCPeerConnectionFactory
    private let audioDevice: AppAudioDevice
    private let videoSource: RTCVideoSource
    private let capturer: RTCVideoCapturer
    private let videoTrack: RTCVideoTrack
    private let audioTrack: RTCAudioTrack?
    private let streamId = UUID().uuidString

    // signaling state, on `queue`
    private var socket: URLSessionWebSocketTask?
    private var code: String?
    private var token: String?
    private var iceServers: [RTCIceServer] = []
    private var viewers: [String: Viewer] = [:]
    private var status = ShareStatus(state: .connecting)
    private var stopped = false
    private var retries = 0
    private var pingTimer: DispatchSourceTimer?
    private var repeatTimer: DispatchSourceTimer?

    // frames, from ReplayKit's thread and the repeat timer
    private let frameLock = NSLock()
    private var live = true
    private var lastFrame: (buffer: CVPixelBuffer, rotation: RTCVideoRotation)?
    private var lastFrameTime: CFTimeInterval = 0

    init(server: URL, password: String, shareAudio: Bool, onFatal: @escaping (String) -> Void) {
        self.server = server
        self.password = password
        self.onFatal = onFatal
        // Viewers show every frame the moment it is decoded (same as the desktop app).
        RTCInitFieldTrialDictionary(["WebRTC-ForceSendPlayoutDelay": "min_ms:0,max_ms:0"])
        RTCInitializeSSL()
        let device = AppAudioDevice()
        audioDevice = device
        factory = RTCPeerConnectionFactory(
            encoderFactory: RTCDefaultVideoEncoderFactory(), // H.264 first: the iPhone's hardware encoder
            decoderFactory: RTCDefaultVideoDecoderFactory(),
            audioDevice: device)
        let source = factory.videoSource()
        // The extension may use about 50 MB: 1080p-sized frames at most, 30 FPS.
        source.adaptOutputFormat(toWidth: 1920, height: 1080, fps: Host.fps)
        videoSource = source
        capturer = RTCVideoCapturer(delegate: source)
        videoTrack = factory.videoTrack(with: source, trackId: "video")
        audioTrack = shareAudio ? factory.audioTrack(with: factory.audioSource(with: nil), trackId: "audio") : nil
        super.init()
    }

    // MARK: lifecycle

    func start() {
        queue.async {
            self.publish()
            self.connect()
            self.startRepeatTimer()
        }
    }

    func setLive(_ on: Bool) {
        frameLock.lock()
        live = on
        frameLock.unlock()
        queue.async {
            self.send(["t": "state", "live": on])
            if self.status.state == .live || self.status.state == .paused {
                self.status.state = on ? .live : .paused
                self.publish()
            }
        }
    }

    /// Closes the room (the viewers see "ended") and everything else.
    func stop() {
        let sent = DispatchSemaphore(value: 0)
        queue.async {
            guard !self.stopped, self.socket != nil else {
                sent.signal()
                return
            }
            self.send(["t": "close"]) { sent.signal() }
        }
        _ = sent.wait(timeout: .now() + 1)
        queue.sync {
            stopped = true
            for viewer in viewers.values { closePeer(viewer) }
            viewers.removeAll()
            pingTimer?.cancel()
            repeatTimer?.cancel()
            socket?.cancel(with: .normalClosure, reason: nil)
            socket = nil
            status = ShareStatus(state: .idle)
            publish()
        }
    }

    private func fail(_ message: String) {
        status.state = .failed
        status.message = message
        publish()
        DispatchQueue.main.async { self.onFatal(message) }
    }

    private func publish() {
        status.save()
    }

    // MARK: media from ReplayKit

    func pushVideo(_ sampleBuffer: CMSampleBuffer) {
        guard let buffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        var rotation = RTCVideoRotation._0
        if let raw = CMGetAttachment(sampleBuffer, key: RPVideoSampleOrientationKey as CFString, attachmentModeOut: nil) as? NSNumber,
           let orientation = CGImagePropertyOrientation(rawValue: raw.uint32Value) {
            switch orientation {
            case .left, .leftMirrored: rotation = ._90
            case .down, .downMirrored: rotation = ._180
            case .right, .rightMirrored: rotation = ._270
            default: break
            }
        }
        deliver(buffer, rotation)
    }

    func pushAudio(_ sampleBuffer: CMSampleBuffer) {
        if audioTrack != nil { audioDevice.push(sampleBuffer) }
    }

    private func deliver(_ buffer: CVPixelBuffer, _ rotation: RTCVideoRotation) {
        let now = CACurrentMediaTime()
        frameLock.lock()
        lastFrame = (buffer, rotation)
        lastFrameTime = now
        let on = live
        frameLock.unlock()
        guard on else { return }
        let frame = RTCVideoFrame(buffer: RTCCVPixelBuffer(pixelBuffer: buffer), rotation: rotation, timeStampNs: Int64(now * 1_000_000_000))
        videoSource.capturer(capturer, didCapture: frame)
    }

    /// iOS sends frames only when the screen changes; a viewer who joins while it is
    /// still would wait for a picture. Repeat the last frame now and then.
    private func startRepeatTimer() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 0.5, repeating: 0.5)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            self.frameLock.lock()
            let frame = self.lastFrame
            let idle = CACurrentMediaTime() - self.lastFrameTime
            self.frameLock.unlock()
            if let frame, idle > 0.9 { self.deliver(frame.buffer, frame.rotation) }
        }
        timer.resume()
        repeatTimer = timer
    }

    // MARK: signaling (WebSocket to the server's /ws)

    private func connect() {
        guard !stopped else { return }
        guard var parts = URLComponents(url: server, resolvingAgainstBaseURL: false) else { return fail("Érvénytelen szervercím.") }
        parts.scheme = parts.scheme == "http" ? "ws" : "wss"
        parts.path = "/ws"
        parts.fragment = nil
        guard let url = parts.url else { return fail("Érvénytelen szervercím.") }
        let task = URLSession.shared.webSocketTask(with: url)
        task.maximumMessageSize = 1 << 20
        socket = task
        task.resume()
        receive(task)
        // take a new room, or the same one back after a dropped connection
        var hello: [String: Any] = ["t": "host", "password": password]
        if let code, let token { hello["resume"] = ["code": code, "token": token] }
        send(hello)
        startPing(task)
    }

    private func receive(_ task: URLSessionWebSocketTask) {
        task.receive { [weak self] result in
            guard let self else { return }
            self.queue.async {
                guard task === self.socket else { return }
                switch result {
                case .failure:
                    self.socketClosed()
                case .success(let message):
                    if case .string(let text) = message, let data = text.data(using: .utf8),
                       let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
                        self.handle(json)
                    }
                    if task === self.socket { self.receive(task) }
                }
            }
        }
    }

    private func send(_ message: [String: Any], done: (() -> Void)? = nil) {
        guard let socket, let data = try? JSONSerialization.data(withJSONObject: message),
              let text = String(data: data, encoding: .utf8)
        else { done?(); return }
        socket.send(.string(text)) { _ in done?() }
    }

    private func startPing(_ task: URLSessionWebSocketTask) {
        pingTimer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 20, repeating: 20)
        timer.setEventHandler { task.sendPing { _ in } }
        timer.resume()
        pingTimer = timer
    }

    private func socketClosed() {
        socket?.cancel(with: .goingAway, reason: nil)
        socket = nil
        pingTimer?.cancel()
        guard !stopped else { return }
        retries += 1
        if retries > 15 { return fail("Megszakadt a kapcsolat a szerverrel.") }
        // the viewers keep watching meanwhile: the media goes directly between the devices
        status.state = .connecting
        status.message = "Újracsatlakozás a szerverhez…"
        publish()
        queue.asyncAfter(deadline: .now() + min(Double(retries) * 2, 10)) { self.connect() }
    }

    private func handle(_ msg: [String: Any]) {
        switch msg["t"] as? String {
        case "hosted":
            retries = 0
            code = msg["code"] as? String
            token = msg["token"] as? String
            iceServers = Self.parseIceServers(msg["iceServers"])
            frameLock.lock()
            let on = live
            frameLock.unlock()
            status.state = on ? .live : .paused
            status.code = code
            status.link = code.map { server.absoluteString + "#" + $0 }
            status.message = nil
            publish()
            send(["t": "state", "live": on])
            let present = Set((msg["viewers"] as? [[String: Any]] ?? []).compactMap { $0["id"] as? String })
            for id in Array(viewers.keys) where !present.contains(id) { dropViewer(id) }
            for id in present { ensureViewer(id) }
        case "viewer":
            if let id = msg["id"] as? String { ensureViewer(id) }
        case "viewer-left":
            if let id = msg["id"] as? String { dropViewer(id) }
        case "signal":
            if let from = msg["from"] as? String, let data = msg["data"] as? [String: Any] { onSignal(from, data) }
        case "count":
            status.viewers = msg["n"] as? Int ?? 0
            publish()
        case "error":
            switch msg["code"] as? String {
            case "auth": fail("Hibás megosztási jelszó – javítsd a Screen Share alkalmazásban.")
            case "rate": fail("Túl sok próbálkozás. Várj egy kicsit, és próbáld újra.")
            case "full": fail("A szerver megtelt. Próbáld újra később.")
            default: break
            }
        case "replaced":
            fail("A szobát egy másik eszköz vette át.")
        default:
            break
        }
    }

    private static func parseIceServers(_ value: Any?) -> [RTCIceServer] {
        (value as? [[String: Any]] ?? []).compactMap { entry in
            let urls = entry["urls"] as? [String] ?? (entry["urls"] as? String).map { [$0] } ?? []
            guard !urls.isEmpty else { return nil }
            return RTCIceServer(urlStrings: urls, username: entry["username"] as? String, credential: entry["credential"] as? String)
        }
    }

    // MARK: viewers

    private func ensureViewer(_ id: String) {
        if let viewer = viewers[id], let pc = viewer.pc, pc.connectionState != .failed, pc.connectionState != .closed { return }
        let viewer = viewers[id] ?? Viewer(id: id)
        viewers[id] = viewer
        createPeer(viewer)
    }

    private func dropViewer(_ id: String) {
        if let viewer = viewers.removeValue(forKey: id) { closePeer(viewer) }
    }

    private func closePeer(_ viewer: Viewer) {
        viewer.reconnect?.cancel()
        if let pc = viewer.pc {
            pc.delegate = nil
            pc.close()
        }
        viewer.pc = nil
    }

    private func createPeer(_ viewer: Viewer) {
        closePeer(viewer)
        viewer.session = String(UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased().prefix(16))
        viewer.createdAt = Date()
        viewer.pendingCandidates = []
        viewer.negotiating = false

        let config = RTCConfiguration()
        config.iceServers = iceServers
        config.sdpSemantics = .unifiedPlan
        config.bundlePolicy = .maxBundle
        let constraints = RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil)
        guard let pc = factory.peerConnection(with: config, constraints: constraints, delegate: self) else { return }
        viewer.pc = pc

        let video = RTCRtpTransceiverInit()
        video.direction = .sendOnly
        video.streamIds = [streamId]
        let videoEncoding = RTCRtpEncodingParameters()
        videoEncoding.maxBitrateBps = NSNumber(value: Host.maxVideoBps)
        videoEncoding.maxFramerate = NSNumber(value: Host.fps)
        videoEncoding.networkPriority = .high
        video.sendEncodings = [videoEncoding]
        _ = pc.addTransceiver(with: videoTrack, init: video)

        if let audioTrack {
            let audio = RTCRtpTransceiverInit()
            audio.direction = .sendOnly
            audio.streamIds = [streamId]
            let audioEncoding = RTCRtpEncodingParameters()
            audioEncoding.maxBitrateBps = NSNumber(value: 510_000)
            audioEncoding.networkPriority = .high
            audio.sendEncodings = [audioEncoding]
            _ = pc.addTransceiver(with: audioTrack, init: audio)
        }
        negotiate(viewer)
    }

    private func negotiate(_ viewer: Viewer) {
        guard let pc = viewer.pc, !viewer.negotiating else { return }
        viewer.negotiating = true
        let constraints = RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil)
        pc.offer(for: constraints) { [weak self] offer, _ in
            guard let self else { return }
            self.queue.async {
                guard viewer.pc === pc, let offer else {
                    viewer.negotiating = false
                    return
                }
                pc.setLocalDescription(offer) { [weak self] error in
                    guard let self else { return }
                    self.queue.async {
                        guard viewer.pc === pc else { return }
                        guard error == nil else {
                            viewer.negotiating = false
                            return
                        }
                        self.send(["t": "signal", "to": viewer.id, "data": ["type": "offer", "session": viewer.session, "sdp": offer.sdp]])
                        // no answer (lost message, viewer stuck): start over
                        self.queue.asyncAfter(deadline: .now() + 15) { [weak self] in
                            guard let self, viewer.pc === pc, pc.signalingState == .haveLocalOffer, self.viewers[viewer.id] === viewer else { return }
                            self.createPeer(viewer)
                        }
                    }
                }
            }
        }
    }

    private func onSignal(_ from: String, _ data: [String: Any]) {
        guard let viewer = viewers[from], let pc = viewer.pc else { return }
        let type = data["type"] as? String
        if type == "recall" {
            if Date().timeIntervalSince(viewer.createdAt) > 5 { createPeer(viewer) }
            return
        }
        guard data["session"] as? String == viewer.session else { return }
        switch type {
        case "answer":
            guard let sdp = data["sdp"] as? String, pc.signalingState == .haveLocalOffer else { return }
            let answer = RTCSessionDescription(type: .answer, sdp: Sdp.tuneAnswer(sdp, startKbps: Host.startKbps, maxKbps: Host.maxKbps))
            pc.setRemoteDescription(answer) { [weak self] error in
                guard let self else { return }
                self.queue.async {
                    guard viewer.pc === pc else { return }
                    viewer.negotiating = false
                    if error != nil { return self.createPeer(viewer) }
                    for candidate in viewer.pendingCandidates { pc.add(candidate) { _ in } }
                    viewer.pendingCandidates = []
                }
            }
        case "candidate":
            guard let info = data["candidate"] as? [String: Any], let sdp = info["candidate"] as? String, !sdp.isEmpty else { return }
            let candidate = RTCIceCandidate(sdp: sdp, sdpMLineIndex: Int32(info["sdpMLineIndex"] as? Int ?? 0), sdpMid: info["sdpMid"] as? String)
            if pc.remoteDescription != nil {
                pc.add(candidate) { _ in }
            } else {
                viewer.pendingCandidates.append(candidate)
            }
        default:
            break
        }
    }

    private func viewer(for pc: RTCPeerConnection) -> Viewer? {
        viewers.values.first { $0.pc === pc }
    }

    private func connectionChanged(_ pc: RTCPeerConnection, _ state: RTCPeerConnectionState) {
        guard let viewer = viewer(for: pc) else { return }
        switch state {
        case .connected:
            viewer.reconnect?.cancel()
        case .disconnected:
            // often recovers by itself; if not, a fresh connection
            let work = DispatchWorkItem { [weak self] in
                guard let self, viewer.pc === pc, pc.connectionState == .disconnected else { return }
                self.createPeer(viewer)
            }
            viewer.reconnect?.cancel()
            viewer.reconnect = work
            queue.asyncAfter(deadline: .now() + 5, execute: work)
        case .failed:
            createPeer(viewer)
        default:
            break
        }
    }
}

extension Host: RTCPeerConnectionDelegate {
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange stateChanged: RTCSignalingState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didAdd stream: RTCMediaStream) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove stream: RTCMediaStream) {}
    func peerConnectionShouldNegotiate(_ peerConnection: RTCPeerConnection) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceConnectionState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceGatheringState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove candidates: [RTCIceCandidate]) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didOpen dataChannel: RTCDataChannel) {}

    func peerConnection(_ peerConnection: RTCPeerConnection, didGenerate candidate: RTCIceCandidate) {
        queue.async {
            guard let viewer = self.viewer(for: peerConnection) else { return }
            var info: [String: Any] = ["candidate": candidate.sdp, "sdpMLineIndex": candidate.sdpMLineIndex]
            if let mid = candidate.sdpMid { info["sdpMid"] = mid }
            self.send(["t": "signal", "to": viewer.id, "data": ["type": "candidate", "session": viewer.session, "candidate": info]])
        }
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCPeerConnectionState) {
        queue.async { self.connectionChanged(peerConnection, newState) }
    }
}
