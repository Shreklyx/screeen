import AVFoundation
import CoreMedia
import WebRTC

/// WebRTC "audio device" that records the sound of the apps on the screen (ReplayKit's
/// .audioApp samples) instead of the microphone. Nothing is played back.
final class AppAudioDevice: NSObject, RTCAudioDevice {
    private static let rate = 48_000.0
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "screenshare.audio", qos: .userInteractive)
    private weak var delegate: RTCAudioDeviceDelegate?
    private var flags = (initialized: false, recordingInitialized: false, recording: false, playoutInitialized: false, playing: false)

    // conversion state, used on `queue` only
    private let outputFormat = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: AppAudioDevice.rate, channels: 2, interleaved: true)!
    private var converter: AVAudioConverter?
    private var converterRate = 0.0

    private func state<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }

    // MARK: RTCAudioDevice

    var deviceInputSampleRate: Double { AppAudioDevice.rate }
    var inputIOBufferDuration: TimeInterval { 0.01 }
    var inputNumberOfChannels: Int { 2 }
    var inputLatency: TimeInterval { 0 }
    var deviceOutputSampleRate: Double { AppAudioDevice.rate }
    var outputIOBufferDuration: TimeInterval { 0.01 }
    var outputNumberOfChannels: Int { 2 }
    var outputLatency: TimeInterval { 0 }

    var isInitialized: Bool { state { flags.initialized } }
    func initialize(with delegate: RTCAudioDeviceDelegate) -> Bool {
        state {
            self.delegate = delegate
            flags.initialized = true
        }
        return true
    }
    func terminateDevice() -> Bool {
        state {
            flags = (false, false, false, false, false)
            delegate = nil
        }
        return true
    }

    var isPlayoutInitialized: Bool { state { flags.playoutInitialized } }
    func initializePlayout() -> Bool { state { flags.playoutInitialized = true }; return true }
    var isPlaying: Bool { state { flags.playing } }
    func startPlayout() -> Bool { state { flags.playing = true }; return true }
    func stopPlayout() -> Bool { state { flags.playing = false }; return true }

    var isRecordingInitialized: Bool { state { flags.recordingInitialized } }
    func initializeRecording() -> Bool { state { flags.recordingInitialized = true }; return true }
    var isRecording: Bool { state { flags.recording } }
    func startRecording() -> Bool { state { flags.recording = true }; return true }
    func stopRecording() -> Bool { state { flags.recording = false }; return true }

    // MARK: ReplayKit → WebRTC

    func push(_ sampleBuffer: CMSampleBuffer) {
        guard isRecording else { return }
        queue.async { self.deliver(sampleBuffer) }
    }

    private func deliver(_ sampleBuffer: CMSampleBuffer) {
        guard let target = state({ flags.recording ? self.delegate : nil }),
              let input = Self.stereoFloat(sampleBuffer)
        else { return }
        if converter == nil || converterRate != input.format.sampleRate {
            converter = AVAudioConverter(from: input.format, to: outputFormat)
            converterRate = input.format.sampleRate
        }
        guard let converter else { return }
        let capacity = AVAudioFrameCount(Double(input.frameLength) * AppAudioDevice.rate / input.format.sampleRate) + 64
        guard let output = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: capacity) else { return }
        var supplied = false
        var error: NSError?
        let status = converter.convert(to: output, error: &error) { _, inputStatus in
            if supplied {
                inputStatus.pointee = .noDataNow
                return nil
            }
            supplied = true
            inputStatus.pointee = .haveData
            return input
        }
        guard status != .error, output.frameLength > 0 else { return }
        var actionFlags = AudioUnitRenderActionFlags()
        var timestamp = AudioTimeStamp()
        timestamp.mHostTime = mach_absolute_time()
        timestamp.mFlags = .hostTimeValid
        _ = target.deliverRecordedData(&actionFlags, &timestamp, 0, output.frameLength, output.audioBufferList, nil, nil)
    }

    /// Any linear PCM ReplayKit hands over (app audio is often 16-bit big-endian) →
    /// native float, two channels.
    private static func stereoFloat(_ sampleBuffer: CMSampleBuffer) -> AVAudioPCMBuffer? {
        guard let description = CMSampleBufferGetFormatDescription(sampleBuffer),
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(description)?.pointee,
              asbd.mFormatID == kAudioFormatLinearPCM, asbd.mChannelsPerFrame > 0, asbd.mSampleRate > 0
        else { return nil }
        let frames = CMSampleBufferGetNumSamples(sampleBuffer)
        guard frames > 0,
              let format = AVAudioFormat(standardFormatWithSampleRate: asbd.mSampleRate, channels: 2),
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames)),
              let out = buffer.floatChannelData
        else { return nil }

        var sizeNeeded = 0
        guard CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, bufferListSizeNeededOut: &sizeNeeded, bufferListOut: nil, bufferListSize: 0,
            blockBufferAllocator: nil, blockBufferMemoryAllocator: nil, flags: 0, blockBufferOut: nil) == noErr
        else { return nil }
        let memory = UnsafeMutableRawPointer.allocate(byteCount: sizeNeeded, alignment: 16)
        defer { memory.deallocate() }
        let list = memory.bindMemory(to: AudioBufferList.self, capacity: 1)
        var block: CMBlockBuffer?
        guard CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, bufferListSizeNeededOut: nil, bufferListOut: list, bufferListSize: sizeNeeded,
            blockBufferAllocator: nil, blockBufferMemoryAllocator: nil,
            flags: kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, blockBufferOut: &block) == noErr
        else { return nil }
        let buffers = UnsafeMutableAudioBufferListPointer(list)

        let channels = Int(asbd.mChannelsPerFrame)
        let nonInterleaved = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        let bigEndian = asbd.mFormatFlags & kAudioFormatFlagIsBigEndian != 0
        let isFloat = asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0
        let bits = Int(asbd.mBitsPerChannel)
        guard (isFloat && bits == 32) || (!isFloat && (bits == 16 || bits == 32)) else { return nil }

        func sample(_ channel: Int, _ frame: Int) -> Float {
            let (bufferIndex, index) = nonInterleaved ? (channel, frame) : (0, frame * channels + channel)
            guard bufferIndex < buffers.count, let data = buffers[bufferIndex].mData else { return 0 }
            if bits == 16 {
                let raw = data.assumingMemoryBound(to: Int16.self)[index]
                return Float(bigEndian ? Int16(bigEndian: raw) : Int16(littleEndian: raw)) / 32768
            }
            let raw = data.assumingMemoryBound(to: UInt32.self)[index]
            let value = bigEndian ? UInt32(bigEndian: raw) : UInt32(littleEndian: raw)
            return isFloat ? Float(bitPattern: value) : Float(Double(Int32(bitPattern: value)) / 2_147_483_648)
        }

        let count = Int(frames)
        for frame in 0..<count {
            let left = sample(0, frame)
            out[0][frame] = left
            out[1][frame] = channels > 1 ? sample(1, frame) : left
        }
        buffer.frameLength = AVAudioFrameCount(count)
        return buffer
    }
}
