import ReplayKit

/// The broadcast extension iOS starts from the Screen Share app or the Control Center:
/// it gets the screen (and the sound of the apps) and shares it like the website does.
final class SampleHandler: RPBroadcastSampleHandler {
    private var host: Host?

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        guard let server = Shared.normalizedServer(Shared.server) else {
            return finish("Nincs beállítva a szerver címe – nyisd meg a Screen Share alkalmazást.")
        }
        let password = Shared.password
        guard !password.isEmpty else {
            return finish("Add meg a megosztási jelszót a Screen Share alkalmazásban, majd indítsd újra.")
        }
        let host = Host(server: server, password: password, shareAudio: Shared.shareAudio) { [weak self] message in
            self?.finish(message)
        }
        self.host = host
        host.start()
    }

    override func broadcastPaused() { host?.setLive(false) }
    override func broadcastResumed() { host?.setLive(true) }

    override func broadcastFinished() {
        host?.stop()
        host = nil
    }

    override func processSampleBuffer(_ sampleBuffer: CMSampleBuffer, with sampleBufferType: RPSampleBufferType) {
        switch sampleBufferType {
        case .video: host?.pushVideo(sampleBuffer)
        case .audioApp: host?.pushAudio(sampleBuffer)
        default: break // the microphone is not shared
        }
    }

    /// Ends the broadcast; iOS shows the message.
    private func finish(_ message: String) {
        host?.stop()
        host = nil
        var status = ShareStatus.load()
        status.state = .failed
        status.message = message
        status.save()
        finishBroadcastWithError(NSError(domain: "ScreenShare", code: 1, userInfo: [NSLocalizedDescriptionKey: message]))
    }
}
