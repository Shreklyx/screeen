#include "host_mac.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>
#include <mach/mach_time.h>

#include <opus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include "clock.h"

namespace ss {

namespace {

double machToMs(uint64_t t) {
  static mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t i;
    mach_timebase_info(&i);
    return i;
  }();
  return double(t) * tb.numer / tb.denom / 1e6;
}

/** A mach time (as ScreenCaptureKit's display times) on the engine's clock. */
double machToNowMs(uint64_t t) { return nowMs() - (machToMs(mach_absolute_time()) - machToMs(t)); }

std::pair<int, int> pixelSize(CGDirectDisplayID id) {
  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
  if (!mode) return {int(CGDisplayPixelsWide(id)), int(CGDisplayPixelsHigh(id))};
  std::pair<int, int> s{int(CGDisplayModeGetPixelWidth(mode)), int(CGDisplayModeGetPixelHeight(mode))};
  CGDisplayModeRelease(mode);
  return s;
}

/** H.264 as the encoder gives it (length-prefixed) → Annex B, with SPS/PPS before a keyframe. */
bool annexB(CMSampleBufferRef sample, std::vector<uint8_t>& out, bool& key) {
  CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
  if (!block) return false;
  key = true;
  CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (atts && CFArrayGetCount(atts) > 0) {
    CFDictionaryRef d = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(atts, 0));
    CFBooleanRef notSync = static_cast<CFBooleanRef>(CFDictionaryGetValue(d, kCMSampleAttachmentKey_NotSync));
    key = !(notSync && CFBooleanGetValue(notSync));
  }
  static const uint8_t start[4] = {0, 0, 0, 1};
  out.clear();
  if (key) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
    size_t count = 0;
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, nullptr);
    for (size_t i = 0; i < count; i++) {
      const uint8_t* p = nullptr;
      size_t n = 0;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &p, &n, nullptr, nullptr) == noErr) {
        out.insert(out.end(), start, start + 4);
        out.insert(out.end(), p, p + n);
      }
    }
  }
  size_t length = CMBlockBufferGetDataLength(block);
  std::vector<uint8_t> bytes(length);
  if (CMBlockBufferCopyDataBytes(block, 0, length, bytes.data()) != noErr) return false;
  for (size_t i = 0; i + 4 <= length;) {
    size_t n = size_t(bytes[i]) << 24 | size_t(bytes[i + 1]) << 16 | size_t(bytes[i + 2]) << 8 | bytes[i + 3];
    i += 4;
    if (!n || i + n > length) break;
    out.insert(out.end(), start, start + 4);
    out.insert(out.end(), bytes.begin() + long(i), bytes.begin() + long(i + n));
    i += n;
  }
  return true;
}

}  // namespace

std::vector<MacDisplay> listDisplays() {
  CGDirectDisplayID ids[16];
  uint32_t count = 0;
  CGGetActiveDisplayList(16, ids, &count);
  std::vector<MacDisplay> out;
  for (uint32_t i = 0; i < count; i++) {
    auto [w, h] = pixelSize(ids[i]);
    bool main = ids[i] == CGMainDisplayID();
    std::string name = std::to_string(i + 1) + ". képernyő (" + std::to_string(w) + "×" + std::to_string(h) + ")" + (main ? " – fő" : "");
    out.push_back({ids[i], name, w, h});
  }
  std::stable_sort(out.begin(), out.end(), [](const MacDisplay& a, const MacDisplay& b) { return a.id == CGMainDisplayID() && b.id != CGMainDisplayID(); });
  return out;
}

}  // namespace ss

/* ------------------------------------------------ ScreenCaptureKit output → the pipeline */

namespace ss {
struct MacHostImplBase {
  virtual ~MacHostImplBase() = default;
  virtual void onVideo(CMSampleBufferRef sample) = 0;
  virtual void onAudio(CMSampleBufferRef sample) = 0;
  virtual void onStopped(NSError* error) = 0;
};
}  // namespace ss

@interface SSStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) std::weak_ptr<ss::MacHostImplBase> owner;
@end

@implementation SSStreamOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
  auto o = self.owner.lock();
  if (!o || !CMSampleBufferIsValid(sample)) return;
  if (type == SCStreamOutputTypeScreen) o->onVideo(sample);
  else o->onAudio(sample);
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  auto o = self.owner.lock();
  if (o) o->onStopped(error);
}
@end

namespace ss {

struct MacHost::Impl : MacHostImplBase, std::enable_shared_from_this<MacHost::Impl> {
  Engine& engine;
  Options opts;
  dispatch_queue_t videoQueue = dispatch_queue_create("ss.video", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  dispatch_queue_t audioQueue = dispatch_queue_create("ss.audio", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  SCStream* stream = nil;
  SSStreamOutput* output = nil;
  VTCompressionSessionRef encoder = nullptr;
  dispatch_source_t timer = nil;
  std::atomic<bool> streaming{false}, wantKey{true}, running{false};
  std::atomic<int> inFlight{0}; // frames in VideoToolbox, not out yet
  int restarts = 0;
  double restartsSince = 0;
  std::atomic<double> bitrate{20e6};
  double sentBitrate = 0;
  // on the video queue
  CVPixelBufferRef lastImage = nullptr;
  double lastEncode = -1e9, lastKey = -1e9, interval = 1000.0 / 60;
  int64_t lastPts = 0;
  int encoded = 0;
  double encSum = 0, statsAt = 0;
  // sound
  std::vector<float> pending, raw;
  double pendingTs = -1;
  OpusEncoder* opus = nullptr;
  mutable std::mutex mu;
  Info info;

  explicit Impl(Engine& e) : engine(e) {}
  ~Impl() override {
    if (encoder) {
      VTCompressionSessionInvalidate(encoder);
      CFRelease(encoder);
    }
    if (lastImage) CVPixelBufferRelease(lastImage);
    if (opus) opus_encoder_destroy(opus);
  }

  void setInfo(const std::function<void(Info&)>& fn) {
    std::lock_guard<std::mutex> l(mu);
    fn(info);
  }

  bool makeEncoder(int w, int h) {
    NSDictionary* spec = @{(__bridge NSString*)kVTVideoEncoderSpecification_EnableLowLatencyRateControl : @YES};
    if (VTCompressionSessionCreate(nullptr, w, h, kCMVideoCodecType_H264, (__bridge CFDictionaryRef)spec, nullptr, nullptr, nullptr, nullptr, &encoder) != noErr) return false;
    auto set = [&](CFStringRef k, id v) { VTSessionSetProperty(encoder, k, (__bridge CFTypeRef)v); };
    set(kVTCompressionPropertyKey_RealTime, @YES);
    set(kVTCompressionPropertyKey_ProfileLevel, (__bridge id)kVTProfileLevel_H264_ConstrainedHigh_AutoLevel);
    set(kVTCompressionPropertyKey_AllowFrameReordering, @NO); // no B-frames
    set(kVTCompressionPropertyKey_MaxKeyFrameInterval, @(opts.fps * 600)); // keyframes when a viewer needs one
    set(kVTCompressionPropertyKey_ExpectedFrameRate, @(opts.fps));
    set(kVTCompressionPropertyKey_AverageBitRate, @(int64_t(bitrate.load())));
    set(kVTCompressionPropertyKey_ColorPrimaries, (__bridge id)kCVImageBufferColorPrimaries_ITU_R_709_2);
    set(kVTCompressionPropertyKey_TransferFunction, (__bridge id)kCVImageBufferTransferFunction_ITU_R_709_2);
    set(kVTCompressionPropertyKey_YCbCrMatrix, (__bridge id)kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    VTCompressionSessionPrepareToEncodeFrames(encoder);
    sentBitrate = bitrate;
    CFBooleanRef hw = nullptr;
    if (VTSessionCopyProperty(encoder, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, nullptr, &hw) == noErr && hw) {
      bool isHw = CFBooleanGetValue(hw);
      CFRelease(hw);
      setInfo([&](Info& i) {
        i.hardware = isHw;
        i.encoder = isHw ? "VideoToolbox" : "szoftveres";
      });
    }
    return true;
  }

  void start(const Options& o) {
    opts = o;
    interval = 1000.0 / o.fps;
    running = true;
    auto self = shared_from_this();
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent* content, NSError* error) {
                                                   if (!self->running) return;
                                                   if (!content) return self->fail(error.code == -3801 ? "permission" : "no-screen-access");
                                                   // on the video queue, as stop() is
                                                   dispatch_async(self->videoQueue, ^{
                                                     if (self->running) self->open(content);
                                                   });
                                                 }];
  }

  void fail(const std::string& why) {
    setInfo([&](Info& i) { i.error = why; });
  }

  void open(SCShareableContent* content) {
    CGDirectDisplayID wanted = opts.display ? opts.display : CGMainDisplayID();
    SCDisplay* display = nil;
    for (SCDisplay* d in content.displays)
      if (d.displayID == wanted) display = d;
    if (!display) return fail("display-not-found");
    auto [srcW, srcH] = pixelSize(display.displayID);
    double scale = 1;
    if (opts.maxWidth > 0 && opts.maxHeight > 0) {
      bool landscape = srcW >= srcH;
      double mw = landscape ? opts.maxWidth : opts.maxHeight, mh = landscape ? opts.maxHeight : opts.maxWidth;
      scale = std::max({1.0, srcW / mw, srcH / mh});
    }
    scale = std::max({scale, srcW / 4096.0, srcH / 4096.0}); // H.264 hardware limit
    int w = std::max(16, int(srcW / scale) & ~1), h = std::max(16, int(srcH / scale) & ~1);
    if (!makeEncoder(w, h)) return fail("no-hw-encoder");
    SCStreamConfiguration* cfg = [SCStreamConfiguration new];
    cfg.width = size_t(w); // scaled on the GPU
    cfg.height = size_t(h);
    // a bit under the frame time, so that a screen frame that comes a hair early (timing jitter) is
    // not left out – that would be a gap, and the next image a frame late
    cfg.minimumFrameInterval = CMTimeMake(9, opts.fps * 10);
    cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange; // what the encoder takes as it is
    cfg.colorMatrix = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    cfg.showsCursor = YES;
    cfg.queueDepth = 4;
    bool audio = false;
    if (@available(macOS 13.0, *)) {
      if (opts.audio) {
        cfg.capturesAudio = YES;
        cfg.excludesCurrentProcessAudio = YES;
        cfg.sampleRate = 48000;
        cfg.channelCount = 2;
        audio = true;
      }
    }
    output = [SSStreamOutput new];
    output.owner = shared_from_this();
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    stream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:output];
    NSError* err = nil;
    if (![stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:videoQueue error:&err]) return fail("capture-failed");
    if (audio) {
      if (@available(macOS 13.0, *)) [stream addStreamOutput:output type:SCStreamOutputTypeAudio sampleHandlerQueue:audioQueue error:nil];
      int e = 0;
      opus = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &e);
      if (opus) opus_encoder_ctl(opus, OPUS_SET_BITRATE(256000));
    }
    setInfo([&](Info& i) {
      i.width = w;
      i.height = h;
      i.audioLayout = audio ? "sztereó" : "";
      i.error.clear(); // (started again after a stop)
    });
    engine.setEncoder("VideoToolbox", true);
    auto self = shared_from_this();
    [stream startCaptureWithCompletionHandler:^(NSError* error) {
      if (error) self->fail(error.code == -3801 ? "permission" : "capture-failed");
    }];
    // a keyframe asked for while the screen stands still: the last image again
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, videoQueue);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 20 * NSEC_PER_MSEC, 2 * NSEC_PER_MSEC);
    std::weak_ptr<Impl> weak = self;
    dispatch_source_set_event_handler(timer, ^{
      auto s = weak.lock();
      if (s && s->streaming && s->wantKey && s->lastImage && nowMs() - s->lastEncode >= s->interval) s->encode(s->lastImage, nowMs());
    });
    dispatch_resume(timer);
  }

  void onVideo(CMSampleBufferRef sample) override {
    if (!running) return;
    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!atts || CFArrayGetCount(atts) == 0) return;
    NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(atts, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) return; // idle / blank frames carry no image
    CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample);
    if (!image) return;
    NSNumber* shown = info[SCStreamFrameInfoDisplayTime];
    double capture = shown ? machToNowMs(shown.unsignedLongLongValue) : nowMs();
    if (lastImage) CVPixelBufferRelease(lastImage);
    lastImage = CVPixelBufferRetain(image);
    if (streaming) encode(image, capture);
  }

  void encode(CVPixelBufferRef image, double capture) {
    // the encoder still works on two: this one is left out – waiting in a queue is only delay (an
    // encoder that cannot keep up, as 4K 60 on a busy or older Mac, would add up a lag otherwise)
    if (inFlight.load() >= 2) return;
    double started = nowMs();
    double bps = bitrate;
    if (std::fabs(bps - sentBitrate) > 1) {
      sentBitrate = bps;
      VTSessionSetProperty(encoder, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef) @(int64_t(bps)));
    }
    // at most 4 keyframes a second (each is big)
    bool key = wantKey && started - lastKey >= 250;
    if (key) {
      wantKey = false;
      lastKey = started;
    }
    lastEncode = started;
    int64_t pts = int64_t(started * 1000);
    if (pts <= lastPts) pts = lastPts + 1;
    lastPts = pts;
    NSDictionary* props = key ? @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES} : nil;
    std::weak_ptr<Impl> weak = shared_from_this();
    inFlight++;
    OSStatus queued = VTCompressionSessionEncodeFrameWithOutputHandler(encoder, image, CMTimeMake(pts, 1000000), kCMTimeInvalid, (__bridge CFDictionaryRef)props, nullptr,
                                                    ^(OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef out) {
                                                      auto s = weak.lock();
                                                      if (s) s->inFlight--;
                                                      if (!s || !s->running || status != noErr || !out) return;
                                                      std::vector<uint8_t> data;
                                                      bool isKey = false;
                                                      if (!annexB(out, data, isKey)) return;
                                                      s->engine.sendVideo(data.data(), data.size(), isKey, capture, double(pts));
                                                      s->stats(nowMs() - started);
                                                    });
    if (queued != noErr) inFlight--;
  }

  void stats(double ms) {
    std::lock_guard<std::mutex> l(mu);
    encSum += ms;
    encoded++;
    double now = nowMs();
    if (now - statsAt >= 1000) {
      info.encodeMs = encSum / std::max(1, encoded);
      info.fps = statsAt ? encoded * 1000.0 / (now - statsAt) : 0;
      engine.setEncoderStats(info.encodeMs, info.width, info.height);
      encSum = 0;
      encoded = 0;
      statsAt = now;
    }
  }

  void onAudio(CMSampleBufferRef sample) override {
    if (!opus || !running) return;
    setInfo([](Info& i) { i.audioBuffers++; });
    // float PCM straight from the block: one channel after the other (non-interleaved, as
    // ScreenCaptureKit gives it) or interleaved – the format description says which
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
    const AudioStreamBasicDescription* asbd = fmt ? CMAudioFormatDescriptionGetStreamBasicDescription(fmt) : nullptr;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!asbd || !block || asbd->mFormatID != kAudioFormatLinearPCM || !(asbd->mFormatFlags & kAudioFormatFlagIsFloat) || asbd->mBitsPerChannel != 32 ||
        std::lround(asbd->mSampleRate) != 48000) {
      if (asbd) {
        char note[96];
        snprintf(note, sizeof note, "format %u/%x/%u bit/%.0f Hz", unsigned(asbd->mFormatID), unsigned(asbd->mFormatFlags), unsigned(asbd->mBitsPerChannel), asbd->mSampleRate);
        setInfo([&](Info& i) { i.audioNote = note; });
      }
      return;
    }
    int channels = std::max<int>(1, int(asbd->mChannelsPerFrame));
    bool planar = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    raw.resize(CMBlockBufferGetDataLength(block) / sizeof(float));
    if (raw.empty() || CMBlockBufferCopyDataBytes(block, 0, raw.size() * sizeof(float), raw.data()) != noErr) return;
    size_t frames = raw.size() / size_t(channels);
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
    double ageMs = (CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock())) - CMTimeGetSeconds(pts)) * 1000;
    // (no time, or one it cannot have: now – an invalid one made every timestamp NaN, no sound)
    if (!CMTIME_IS_NUMERIC(pts) || !(ageMs >= 0 && ageMs < 1000)) ageMs = 0;
    if (!(pendingTs >= 0) || pending.empty()) pendingTs = (nowMs() - ageMs) * 1000;
    for (size_t i = 0; i < frames; i++) {
      for (int c = 0; c < 2; c++) {
        size_t ch = size_t(std::min(c, channels - 1));
        pending.push_back(planar ? raw[ch * frames + i] : raw[i * size_t(channels) + ch]);
      }
    }
    uint8_t out[1500];
    while (pending.size() >= 240) {
      // 2.5 ms packets: the least delay Opus has
      int n = opus_encode_float(opus, pending.data(), 120, out, sizeof out);
      if (n > 0) {
        engine.sendAudio(0, 2, out, size_t(n), pendingTs);
        setInfo([](Info& i) { i.audioPackets++; });
      } else {
        setInfo([&](Info& i) { i.audioNote = std::string("opus: ") + opus_strerror(n); });
      }
      pending.erase(pending.begin(), pending.begin() + 240);
      pendingTs += 2500;
    }
  }

  void onStopped(NSError* error) override {
    // not allowed (-3801), or stopped by the person from the menu bar (-3817): that is the end
    if (error.code == -3801 || error.code == -3817 || !running) return fail(error.code == -3801 ? "permission" : "capture-stopped");
    // stopped by the system (the display changed or woke, the sound's capture failed…): the picture
    // and the sound start again in a second – a few times a minute at most
    double now = nowMs();
    if (now - restartsSince > 60000) {
      restarts = 0;
      restartsSince = now;
    }
    if (++restarts > 5) return fail("capture-stopped");
    auto self = shared_from_this();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), videoQueue, ^{
      if (!self->running) return;
      if (self->timer) dispatch_source_cancel(self->timer);
      self->timer = nil;
      self->stream = nil;
      if (self->encoder) {
        VTCompressionSessionInvalidate(self->encoder);
        CFRelease(self->encoder);
        self->encoder = nullptr;
      }
      self->inFlight = 0;
      dispatch_sync(self->audioQueue, ^{ // (the sound's encoder is used there)
        if (self->opus) opus_encoder_destroy(self->opus);
        self->opus = nullptr;
        self->pending.clear();
        self->pendingTs = -1;
      });
      self->wantKey = true;
      self->start(self->opts);
    });
  }

  void stop() {
    running = false;
    streaming = false;
    // on the video queue: whatever runs there finishes first, nothing reaches the engine after
    dispatch_sync(videoQueue, ^{
      if (timer) dispatch_source_cancel(timer);
      timer = nil;
      if (stream) [stream stopCaptureWithCompletionHandler:nil];
      stream = nil;
    });
    dispatch_sync(audioQueue, ^{});
    if (encoder) VTCompressionSessionCompleteFrames(encoder, kCMTimeInvalid);
  }
};

MacHost::MacHost(Engine& engine) : impl_(std::make_shared<Impl>(engine)) {}
MacHost::~MacHost() { stop(); }

void MacHost::start(const Options& options) { impl_->start(options); }  // (macOS 12.3+: the app's minimum)

void MacHost::stop() { impl_->stop(); }

void MacHost::setStreaming(bool on) {
  impl_->streaming = on;
  if (on) impl_->wantKey = true;
}

void MacHost::keyframe() { impl_->wantKey = true; }
void MacHost::setBitrate(double bps) { impl_->bitrate = bps; }

MacHost::Info MacHost::info() const {
  std::lock_guard<std::mutex> l(impl_->mu);
  return impl_->info;
}

}  // namespace ss
