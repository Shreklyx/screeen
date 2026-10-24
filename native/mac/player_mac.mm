#include "player_mac.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "audio_ring.h"
#include "clock.h"

namespace ss {

/* ------------------------------------------------ picture */

struct MacPlayer::Impl : std::enable_shared_from_this<MacPlayer::Impl> {
  MacPlayer* owner = nullptr;
  CALayer* layer = nil;
  dispatch_queue_t queue = dispatch_queue_create("ss.decode", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  CMVideoFormatDescriptionRef format = nullptr;
  VTDecompressionSessionRef session = nullptr;
  std::vector<uint8_t> sps, pps;
  bool needKey = true;
  CVPixelBufferRef shown = nullptr; // kept while it is on screen
  // statistics
  mutable std::mutex mu;
  Stats stats;
  double rawLatency = NAN;
  std::vector<double> lat; // arrival − capture on our clock; the offset is added in stats()
  int frames = 0;
  double statsAt = 0;

  ~Impl() {
    if (session) {
      VTDecompressionSessionInvalidate(session);
      CFRelease(session);
    }
    if (format) CFRelease(format);
    if (shown) CVPixelBufferRelease(shown);
  }

  void keyframe() {
    needKey = true;
    if (owner && owner->needKeyframe) owner->needKeyframe();
  }

  bool makeSession() {
    if (session) {
      VTDecompressionSessionInvalidate(session);
      CFRelease(session);
      session = nullptr;
    }
    const uint8_t* sets[2] = {sps.data(), pps.data()};
    size_t sizes[2] = {sps.size(), pps.size()};
    CMVideoFormatDescriptionRef f = nullptr;
    if (CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, 2, sets, sizes, 4, &f) != noErr) return false;
    if (format) CFRelease(format);
    format = f;
    NSDictionary* spec = @{(__bridge NSString*)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder : @YES};
    // BGRA in an IOSurface: the layer shows it as it is
    NSDictionary* attrs = @{
      (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    if (VTDecompressionSessionCreate(nullptr, format, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)attrs, nullptr, &session) != noErr) return false;
    VTSessionSetProperty(session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
    CFBooleanRef hw = nullptr;
    bool isHw = true;
    if (VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, nullptr, &hw) == noErr && hw) {
      isHw = CFBooleanGetValue(hw);
      CFRelease(hw);
    }
    std::lock_guard<std::mutex> l(mu);
    stats.hardware = isHw;
    return true;
  }

  void decode(const std::vector<uint8_t>& data, bool key, double capture) {
    if (needKey && !key) return keyframe();
    // Annex B → NAL units; SPS/PPS make the format, the rest goes length-prefixed (AVCC)
    std::vector<uint8_t> avcc;
    bool newSets = false;
    size_t i = 0, n = data.size();
    auto nextStart = [&](size_t from) {
      for (size_t k = from; k + 3 <= n; k++)
        if (data[k] == 0 && data[k + 1] == 0 && (data[k + 2] == 1 || (k + 3 < n && data[k + 2] == 0 && data[k + 3] == 1))) return k;
      return n;
    };
    i = nextStart(0);
    while (i < n) {
      size_t start = i + (data[i + 2] == 1 ? 3 : 4);
      size_t end = nextStart(start);
      size_t len = end - start;
      while (len > 0 && data[start + len - 1] == 0 && end < n) len--; // trailing zeros of a 4-byte start code
      if (len > 0) {
        int type = data[start] & 0x1F;
        if (type == 7 || type == 8) {
          std::vector<uint8_t>& dst = type == 7 ? sps : pps;
          std::vector<uint8_t> v(data.begin() + long(start), data.begin() + long(start + len));
          if (v != dst) {
            dst = v;
            newSets = true;
          }
        } else if (type != 9) {
          uint8_t h[4] = {uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len)};
          avcc.insert(avcc.end(), h, h + 4);
          avcc.insert(avcc.end(), data.begin() + long(start), data.begin() + long(start + len));
        }
      }
      i = end;
    }
    if ((newSets || !session) && !sps.empty() && !pps.empty() && !makeSession()) return keyframe();
    if (!session || avcc.empty()) return;
    needKey = false;
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, avcc.size(), nullptr, nullptr, 0, avcc.size(), 0, &block) != noErr) return;
    CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
    CMSampleBufferRef sample = nullptr;
    size_t size = avcc.size();
    CMSampleBufferCreateReady(nullptr, block, format, 1, 0, nullptr, 1, &size, &sample);
    CFRelease(block);
    if (!sample) return;
    std::weak_ptr<Impl> weak = shared_from_this();
    VTDecodeInfoFlags flags = 0;
    OSStatus st = VTDecompressionSessionDecodeFrameWithOutputHandler(session, sample, 0, &flags,
                                                                    ^(OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
                                                                      auto s = weak.lock();
                                                                      if (!s) return;
                                                                      // (it may run on VideoToolbox's thread: the keyframe request goes via the queue)
                                                                      if (status != noErr || !image) return dispatch_async(s->queue, ^{ s->keyframe(); });
                                                                      s->show(image, capture);
                                                                    });
    CFRelease(sample);
    if (st != noErr) keyframe();
  }

  void show(CVImageBufferRef image, double capture) {
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(image);
    if (!surface) return;
    CVPixelBufferRetain(image);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contents = (__bridge id)surface;
    [CATransaction commit];
    [CATransaction flush];
    if (shown) CVPixelBufferRelease(shown);
    shown = image;
    double now = nowMs();
    std::lock_guard<std::mutex> l(mu);
    if (capture > 0) lat.push_back(now - capture);
    frames++;
    stats.width = int(CVPixelBufferGetWidth(image));
    stats.height = int(CVPixelBufferGetHeight(image));
    if (now - statsAt >= 1000) {
      stats.fps = statsAt ? frames * 1000.0 / (now - statsAt) : 0;
      if (!lat.empty()) {
        std::nth_element(lat.begin(), lat.begin() + long(lat.size() / 2), lat.end());
        rawLatency = lat[lat.size() / 2];
      }
      lat.clear();
      frames = 0;
      statsAt = now;
    }
  }
};

MacPlayer::MacPlayer(CALayer* layer) : impl_(std::make_shared<Impl>()) {
  impl_->owner = this;
  impl_->layer = layer;
  layer.contentsGravity = kCAGravityResizeAspect; // the whole picture, its aspect kept
  layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
}

MacPlayer::~MacPlayer() { detach(); }

void MacPlayer::detach() {
  // on the decoder's queue: whatever runs there now finishes first
  Impl* impl = impl_.get();
  dispatch_sync(impl->queue, ^{ impl->owner = nullptr; });
}

void MacPlayer::push(const uint8_t* data, size_t n, bool key, double captureMs) {
  auto copy = std::make_shared<std::vector<uint8_t>>(data, data + n);
  std::weak_ptr<Impl> weak = impl_;
  dispatch_async(impl_->queue, ^{
    auto s = weak.lock();
    if (s) s->decode(*copy, key, captureMs);
  });
}

MacPlayer::Stats MacPlayer::stats() const {
  Stats st;
  double raw;
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    st = impl_->stats;
    raw = impl_->rawLatency;
  }
  double off = clockOffset ? clockOffset() : NAN;
  if (!std::isnan(raw) && !std::isnan(off)) st.latencyMs = raw + off;
  return st;
}

/* ------------------------------------------------ sound */

struct MacAudioOut::Impl {
  AudioRing ring;
  AudioUnit unit = nullptr;
  int channels = 2;

  static OSStatus render(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io) {
    auto* self = static_cast<Impl*>(ref);
    self->ring.pull(static_cast<float*>(io->mBuffers[0].mData), self->channels, int(frames));
    return noErr;
  }

  bool open() {
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &d);
    if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr) return false;
    // the device's channel count (2 for most; 6 / 8 for surround outputs)
    AudioStreamBasicDescription dev = {};
    UInt32 size = sizeof dev;
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &dev, &size) == noErr && dev.mChannelsPerFrame >= 1)
      channels = int(std::min<UInt32>(8, dev.mChannelsPerFrame));
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = 48000;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = UInt32(channels);
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = fmt.mBytesPerPacket = UInt32(4 * channels);
    fmt.mFramesPerPacket = 1;
    AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
    AURenderCallbackStruct cb = {render, this};
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    // a small device buffer: ~2.7 ms
    UInt32 frames = 128;
    AudioUnitSetProperty(unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames, sizeof frames);
    return AudioUnitInitialize(unit) == noErr && AudioOutputUnitStart(unit) == noErr;
  }

  ~Impl() {
    if (unit) {
      AudioOutputUnitStop(unit);
      AudioUnitUninitialize(unit);
      AudioComponentInstanceDispose(unit);
    }
  }
};

MacAudioOut::MacAudioOut() : impl_(new Impl) { impl_->open(); }
MacAudioOut::~MacAudioOut() = default;
void MacAudioOut::push(int sub, const uint8_t* data, size_t n, double tsUs) { impl_->ring.push(sub, data, n, tsUs); }
void MacAudioOut::setVolume(float v) { impl_->ring.setVolume(v); }

}  // namespace ss
