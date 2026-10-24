// ss-capture-mac – native screen capture and hardware H.264 encoding for direct mode on macOS,
// the counterpart of ss-capture.cpp (Windows + NVENC):
//
//  - ScreenCaptureKit hands over each new image of the display the moment it is composed,
//    already scaled to the sent size and converted to YUV on the GPU (an IOSurface),
//  - VideoToolbox encodes that surface directly on the media engine – no copy through the
//    processor: low-latency rate control, no B-frames, keyframes only when asked for,
//  - the encoded frames go to stdout; the app (direct.js) sends them over UDP at once.
//
//   ss-capture-mac [--display id] [--width W --height H] [--fps N] [--bitrate bps]
//   ss-capture-mac --probe       JSON: displays, hardware encoder – then exit
//
// stdout: packets – 4-byte tag, uint32 little-endian payload size, payload (as ss-capture.exe)
//   "INF1"  uint32 width, height, fps, sourceWidth, sourceHeight, then the GPU's name (UTF-8)
//   "VID1"  uint8 keyframe, 3 bytes 0, uint32 encoding time (µs), float64 capture time (ms since
//           1970, when the image was composed), H.264 Annex B
//   "ERR1"  UTF-8 text, then exit (permission, no-hw-encoder, display-not-found, …)
// stdin: lines – "K" keyframe, "B <bits/s>" bitrate; closing stdin ends the capture.

import CoreGraphics
import CoreMedia
import CoreVideo
import Foundation
import Metal
import ScreenCaptureKit
import VideoToolbox

/* ------------------------------------------------------------------ output */

let outLock = NSLock()

func writeAll(_ data: Data) {
  data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
    guard var p = raw.baseAddress else { return }
    var left = raw.count
    while left > 0 {
      let n = write(1, p, left)
      if n <= 0 { exit(0) } // the app is gone
      p = p.advanced(by: n)
      left -= n
    }
  }
}

extension Data {
  mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
    var v = value.littleEndian
    Swift.withUnsafeBytes(of: &v) { append(contentsOf: $0) }
  }

  mutating func appendDouble(_ value: Double) {
    appendLE(value.bitPattern)
  }
}

func sendPacket(_ tag: String, _ body: Data) {
  var packet = Data(tag.utf8)
  packet.appendLE(UInt32(body.count))
  packet.append(body)
  outLock.lock()
  writeAll(packet)
  outLock.unlock()
}

func fail(_ text: String) -> Never {
  sendPacket("ERR1", Data(text.utf8))
  exit(1)
}

/* ------------------------------------------------------------------ clocks */

var timebase = mach_timebase_info_data_t()
mach_timebase_info(&timebase)
func machToMs(_ t: UInt64) -> Double { Double(t) * Double(timebase.numer) / Double(timebase.denom) / 1e6 }
func epochNowMs() -> Double { Date().timeIntervalSince1970 * 1000 }
func nowMs() -> Double { machToMs(mach_absolute_time()) }
let epochAtMach0 = epochNowMs() - nowMs()

/* ------------------------------------------------------------------ options */

struct Options {
  var display: CGDirectDisplayID?
  var width = 0 // the largest picture to send (keeps the aspect ratio); 0 = as the screen
  var height = 0
  var fps = 60
  var bitrate = 50_000_000
  var probe = false
}

func parseArgs() -> Options {
  var o = Options()
  let a = CommandLine.arguments
  var i = 1
  while i < a.count {
    let value = i + 1 < a.count ? a[i + 1] : ""
    switch a[i] {
    case "--probe": o.probe = true
    case "--display": o.display = UInt32(value); i += 1
    case "--width": o.width = Int(value) ?? 0; i += 1
    case "--height": o.height = Int(value) ?? 0; i += 1
    case "--fps": o.fps = max(1, min(240, Int(value) ?? 60)); i += 1
    case "--bitrate": o.bitrate = max(500_000, min(800_000_000, Int(value) ?? 50_000_000)); i += 1
    default: break
    }
    i += 1
  }
  return o
}

/** The display's size in pixels (a Retina display has more than its points). */
func pixelSize(_ id: CGDirectDisplayID) -> (Int, Int) {
  if let mode = CGDisplayCopyDisplayMode(id) { return (mode.pixelWidth, mode.pixelHeight) }
  return (Int(CGDisplayPixelsWide(id)), Int(CGDisplayPixelsHigh(id)))
}

func targetSize(_ o: Options, _ srcW: Int, _ srcH: Int) -> (Int, Int) {
  var scale = 1.0
  if o.width > 0 && o.height > 0 {
    let landscape = srcW >= srcH
    let maxW = Double(landscape ? o.width : o.height)
    let maxH = Double(landscape ? o.height : o.width)
    scale = max(1, Double(srcW) / maxW, Double(srcH) / maxH)
  }
  scale = max(scale, Double(srcW) / 4096, Double(srcH) / 4096) // H.264 hardware limit
  return (max(16, Int(Double(srcW) / scale) & ~1), max(16, Int(Double(srcH) / scale) & ~1))
}

/* ------------------------------------------------------------------ VideoToolbox */

func makeEncoder(width: Int, height: Int, fps: Int, bitrate: Int) -> (VTCompressionSession?, String) {
  let spec: [CFString: Any] = [
    kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true,
    kVTVideoEncoderSpecification_EnableLowLatencyRateControl: true, // no frame waits in the encoder
  ]
  var created: VTCompressionSession?
  let status = VTCompressionSessionCreate(
    allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264,
    encoderSpecification: spec as CFDictionary, imageBufferAttributes: nil, compressedDataAllocator: nil,
    outputCallback: nil, refcon: nil, compressionSessionOut: &created)
  guard status == noErr, let session = created else { return (nil, "no-hw-encoder") }
  let set = { (key: CFString, value: Any) in _ = VTSessionSetProperty(session, key: key, value: value as AnyObject) }
  set(kVTCompressionPropertyKey_RealTime, true)
  set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel)
  set(kVTCompressionPropertyKey_AllowFrameReordering, false) // no B-frames
  set(kVTCompressionPropertyKey_MaxKeyFrameInterval, fps * 600) // keyframes when a viewer needs one
  set(kVTCompressionPropertyKey_ExpectedFrameRate, fps)
  set(kVTCompressionPropertyKey_AverageBitRate, bitrate)
  // the screen is converted with BT.709 (what the decoders assume for HD video)
  set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
  set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
  set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
  VTCompressionSessionPrepareToEncodeFrames(session)
  return (session, "")
}

/** H.264 as the encoder gives it (length-prefixed) → Annex B, with SPS/PPS before a keyframe. */
func annexB(_ sample: CMSampleBuffer) -> (Data, Bool)? {
  guard let block = CMSampleBufferGetDataBuffer(sample) else { return nil }
  var key = true
  if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [NSDictionary],
     let first = attachments.first, let notSync = first[kCMSampleAttachmentKey_NotSync] as? Bool {
    key = !notSync
  }
  let startCode: [UInt8] = [0, 0, 0, 1]
  var out = Data()
  if key, let format = CMSampleBufferGetFormatDescription(sample) {
    var count = 0
    _ = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                           parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
    for i in 0..<count {
      var pointer: UnsafePointer<UInt8>?
      var size = 0
      if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: i, parameterSetPointerOut: &pointer,
                                                            parameterSetSizeOut: &size, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr,
         let pointer = pointer {
        out.append(contentsOf: startCode)
        out.append(pointer, count: size)
      }
    }
  }
  let length = CMBlockBufferGetDataLength(block)
  var bytes = [UInt8](repeating: 0, count: length)
  guard CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: &bytes) == noErr else { return nil }
  var i = 0
  while i + 4 <= length {
    let n = Int(bytes[i]) << 24 | Int(bytes[i + 1]) << 16 | Int(bytes[i + 2]) << 8 | Int(bytes[i + 3])
    i += 4
    guard n > 0, i + n <= length else { break }
    out.append(contentsOf: startCode)
    out.append(contentsOf: bytes[i ..< i + n])
    i += n
  }
  return (out, key)
}

/* ------------------------------------------------------------------ capture */

final class Capturer: NSObject, SCStreamOutput, SCStreamDelegate {
  let queue = DispatchQueue(label: "ss-capture", qos: .userInteractive)
  let encoder: VTCompressionSession
  let interval: Double
  var scStream: SCStream?
  var timer: DispatchSourceTimer?
  var wantKey = true // the first frame is one
  var lastImage: CVPixelBuffer?
  var lastCapture = 0.0
  var lastEncode = -1e9
  var lastKey = -1e9
  var lastPts: Int64 = 0

  init(encoder: VTCompressionSession, fps: Int) {
    self.encoder = encoder
    interval = 1000 / Double(fps)
    super.init()
  }

  func start() {
    // a keyframe asked for while the screen stands still: the last image again
    let t = DispatchSource.makeTimerSource(queue: queue)
    t.schedule(deadline: .now(), repeating: .milliseconds(20))
    t.setEventHandler { [weak self] in
      guard let self = self, self.wantKey, let image = self.lastImage, nowMs() - self.lastEncode >= self.interval else { return }
      self.encode(image, capture: epochNowMs())
    }
    t.resume()
    timer = t
  }

  func requestKeyframe() { queue.async { self.wantKey = true } }

  func setBitrate(_ bps: Int) {
    queue.async { _ = VTSessionSetProperty(self.encoder, key: kVTCompressionPropertyKey_AverageBitRate, value: bps as AnyObject) }
  }

  func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
    guard type == .screen, CMSampleBufferIsValid(sample),
          let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
          let info = attachments.first,
          let raw = info[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete, // not an idle / blank frame
          let image = CMSampleBufferGetImageBuffer(sample) else { return }
    // when the image was composed
    let shown = (info[.displayTime] as? NSNumber)?.uint64Value
    let capture = shown.map { epochAtMach0 + machToMs($0) } ?? epochNowMs()
    lastImage = image
    lastCapture = capture
    encode(image, capture: capture)
  }

  func stream(_ stream: SCStream, didStopWithError error: Error) {
    fail((error as NSError).code == -3801 ? "permission" : "capture-stopped")
  }

  func encode(_ image: CVPixelBuffer, capture: Double) {
    let started = nowMs()
    // at most 4 keyframes a second (each is big)
    let key = wantKey && started - lastKey >= 250
    if key {
      wantKey = false
      lastKey = started
    }
    lastEncode = started
    var pts = Int64(started * 1000)
    if pts <= lastPts { pts = lastPts + 1 }
    lastPts = pts
    let props: CFDictionary? = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
    let status = VTCompressionSessionEncodeFrame(
      encoder, imageBuffer: image, presentationTimeStamp: CMTime(value: pts, timescale: 1_000_000), duration: .invalid,
      frameProperties: props, infoFlagsOut: nil
    ) { status, _, sample in
      guard status == noErr, let sample = sample, let encoded = annexB(sample) else { return }
      let (data, isKey) = encoded
      var body = Data()
      body.append(UInt8(isKey ? 1 : 0))
      body.append(contentsOf: [0, 0, 0])
      body.appendLE(UInt32(max(0, (nowMs() - started) * 1000)))
      body.appendDouble(capture)
      body.append(data)
      sendPacket("VID1", body)
    }
    if status != noErr { fail("encode-failed") }
  }
}

/* ------------------------------------------------------------------ probe */

func probe() {
  var ids = [CGDirectDisplayID](repeating: 0, count: 16)
  var count: UInt32 = 0
  CGGetActiveDisplayList(16, &ids, &count)
  let displays = ids.prefix(Int(count)).map { id -> String in
    let (w, h) = pixelSize(id)
    return "{\"id\":\(id),\"width\":\(w),\"height\":\(h),\"main\":\(id == CGMainDisplayID())}"
  }
  let (session, error) = makeEncoder(width: 1920, height: 1080, fps: 60, bitrate: 20_000_000)
  if let session = session { VTCompressionSessionInvalidate(session) }
  let gpu = (MTLCreateSystemDefaultDevice()?.name ?? "").replacingOccurrences(of: "\"", with: "")
  print("{\"hwEncoder\":\(session != nil),\"encoderError\":\"\(error)\",\"gpu\":\"\(gpu)\",\"displays\":[\(displays.joined(separator: ","))]}")
}

/* ------------------------------------------------------------------ main */

let opts = parseArgs()
if opts.probe {
  probe()
  exit(0)
}
signal(SIGPIPE, SIG_IGN) // a closed pipe: write() fails and we exit

var capturer: Capturer?

// "K" → keyframe, "B 80000000" → bitrate; stdin closed (the app stopped us) → quit
Thread.detachNewThread {
  var line = ""
  while true {
    let chunk = FileHandle.standardInput.availableData
    if chunk.isEmpty { exit(0) }
    for byte in chunk {
      if byte != 10 {
        if line.count < 64 { line.append(Character(UnicodeScalar(byte))) }
        continue
      }
      if line == "K" { capturer?.requestKeyframe() }
      else if line.hasPrefix("B "), let bps = Int(line.dropFirst(2)) { capturer?.setBitrate(bps) }
      line = ""
    }
  }
}

SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { content, error in
  guard let content = content else {
    fail((error as NSError?)?.code == -3801 ? "permission" : "no-screen-access")
  }
  let wanted = opts.display ?? CGMainDisplayID()
  guard let display = content.displays.first(where: { $0.displayID == wanted }) else { fail("display-not-found") }
  let (srcW, srcH) = pixelSize(display.displayID)
  let (outW, outH) = targetSize(opts, srcW, srcH)
  let (session, encoderError) = makeEncoder(width: outW, height: outH, fps: opts.fps, bitrate: opts.bitrate)
  guard let encoder = session else { fail(encoderError) }

  let config = SCStreamConfiguration()
  config.width = outW // scaled on the GPU
  config.height = outH
  config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(opts.fps))
  config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange // what the encoder takes as it is
  config.colorMatrix = kCVImageBufferYCbCrMatrix_ITU_R_709_2
  config.showsCursor = true
  config.queueDepth = 4

  let c = Capturer(encoder: encoder, fps: opts.fps)
  let stream = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: config, delegate: c)
  do {
    try stream.addStreamOutput(c, type: .screen, sampleHandlerQueue: c.queue)
  } catch {
    fail("capture-failed")
  }
  c.scStream = stream
  capturer = c
  stream.startCapture { error in
    if let error = error { fail((error as NSError).code == -3801 ? "permission" : "capture-failed") }
    var info = Data()
    for v in [outW, outH, opts.fps, srcW, srcH] { info.appendLE(UInt32(v)) }
    info.append(Data((MTLCreateSystemDefaultDevice()?.name ?? "Apple").utf8))
    sendPacket("INF1", info)
    c.start()
  }
}

dispatchMain()
