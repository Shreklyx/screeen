#include "player_mac.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <CoreVideo/CVMetalTexture.h>
#import <CoreVideo/CVMetalTextureCache.h>
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

namespace {
// the decoded frame (BGRA) letterboxed into the drawable; fades in from black at the start. Scaled
// with care: smaller on the screen, each screen pixel is the average of the picture's pixels under it
// (one bilinear sample shimmers and makes text ragged); larger, bicubic (Catmull-Rom, sharper than
// bilinear); at 1:1 each pixel is the picture's own. look: the fade (x), picture pixels per screen pixel (zw)
NSString* const kShader = @"#include <metal_stdlib>\n"
                           "using namespace metal;\n"
                           "struct V { float4 pos [[position]]; float2 uv; };\n"
                           "constexpr sampler smp(filter::linear, address::clamp_to_edge);\n"
                           "vertex V vs(uint id [[vertex_id]], constant float4& rect [[buffer(0)]]) {\n"
                           "  float2 c = float2(float(id & 1), float(id >> 1));\n"
                           "  V o;\n"
                           "  o.pos = float4(mix(rect.x, rect.z, c.x) * 2 - 1, 1 - mix(rect.y, rect.w, c.y) * 2, 0, 1);\n"
                           "  o.uv = c;\n"
                           "  return o;\n"
                           "}\n"
                           "float3 bicubic(texture2d<float> t, float2 uv, float2 size) {\n"
                           "  float2 p = uv * size, t1 = floor(p - 0.5) + 0.5, f = p - t1;\n"
                           "  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
                           "  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f), w12 = w1 + w2;\n"
                           "  float2 a = (t1 - 1) / size, b = (t1 + w2 / w12) / size, c = (t1 + 2) / size;\n"
                           "  return (t.sample(smp, float2(a.x, a.y)).rgb * w0.x + t.sample(smp, float2(b.x, a.y)).rgb * w12.x + t.sample(smp, float2(c.x, a.y)).rgb * w3.x) * w0.y +\n"
                           "         (t.sample(smp, float2(a.x, b.y)).rgb * w0.x + t.sample(smp, float2(b.x, b.y)).rgb * w12.x + t.sample(smp, float2(c.x, b.y)).rgb * w3.x) * w12.y +\n"
                           "         (t.sample(smp, float2(a.x, c.y)).rgb * w0.x + t.sample(smp, float2(b.x, c.y)).rgb * w12.x + t.sample(smp, float2(c.x, c.y)).rgb * w3.x) * w3.y;\n"
                           "}\n"
                           "fragment float4 fs(V i [[stage_in]], texture2d<float> tex [[texture(0)]], constant float4& look [[buffer(0)]]) {\n"
                           "  float2 size = float2(float(tex.get_width()), float(tex.get_height())), r = look.zw;\n"
                           "  float3 c = float3(0);\n"
                           "  if (max(r.x, r.y) > 1.01) {\n"
                           "    int2 n = int2(clamp(ceil(r - 0.01), float2(1), float2(4)));\n"
                           "    float2 gap = r / (float2(n) * size), start = i.uv - gap * (float2(n) - 1) * 0.5;\n"
                           "    for (int b = 0; b < n.y; b++)\n"
                           "      for (int a = 0; a < n.x; a++) c += tex.sample(smp, start + gap * float2(float(a), float(b))).rgb;\n"
                           "    c /= float(n.x * n.y);\n"
                           "  } else if (min(r.x, r.y) < 0.99) {\n"
                           "    c = bicubic(tex, i.uv, size);\n"
                           "  } else {\n"
                           "    c = tex.sample(smp, i.uv).rgb;\n"
                           "  }\n"
                           "  return float4(saturate(c) * look.x, 1);\n"
                           "}\n";
}  // namespace

struct MacPlayer::Impl : std::enable_shared_from_this<MacPlayer::Impl> {
  MacPlayer* owner = nullptr;
  CALayer* layer = nil;
  dispatch_queue_t queue = dispatch_queue_create("ss.decode", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  CMVideoFormatDescriptionRef format = nullptr;
  VTDecompressionSessionRef session = nullptr;
  std::vector<uint8_t> sps, pps;
  bool needKey = true;
  CVPixelBufferRef shown = nullptr; // kept while it is on screen
  // Metal: each frame goes to the screen at once – not held for the display's next refresh
  // (in full screen that is up to a frame less delay); layer.contents when there is no Metal
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> commands = nil;
  id<MTLRenderPipelineState> pipeline = nil;
  CAMetalLayer* metal = nil;
  CVMetalTextureCacheRef cache = nullptr;
  double firstAt = 0;
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
    if (cache) CFRelease(cache);
  }

  /** On the main thread, at the start. */
  bool makeMetal(CALayer* host) {
    device = MTLCreateSystemDefaultDevice();
    if (!device) return false;
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:kShader options:nil error:&err];
    if (!lib) return false;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline = [device newRenderPipelineStateWithDescriptor:d error:&err];
    commands = [device newCommandQueue];
    if (!pipeline || !commands || CVMetalTextureCacheCreate(nullptr, nullptr, device, nullptr, &cache) != kCVReturnSuccess) return false;
    metal = [CAMetalLayer layer];
    metal.device = device;
    metal.pixelFormat = MTLPixelFormatBGRA8Unorm;
    metal.framebufferOnly = YES;
    metal.opaque = YES;
    metal.displaySyncEnabled = NO; // present now (it may tear rather than wait)
    metal.maximumDrawableCount = 2;
    metal.allowsNextDrawableTimeout = YES;
    metal.contentsScale = NSScreen.mainScreen.backingScaleFactor ?: 2;
    metal.actions = @{@"bounds" : NSNull.null, @"position" : NSNull.null, @"frame" : NSNull.null, @"contents" : NSNull.null};
    metal.frame = host.bounds;
    metal.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [host addSublayer:metal];
    return true;
  }

  bool drawMetal(CVPixelBufferRef image) {
    if (!metal || !pipeline) return false;
    CGSize size = metal.bounds.size;
    CGFloat scale = metal.contentsScale;
    CGSize px = CGSizeMake(std::max<CGFloat>(1, std::round(size.width * scale)), std::max<CGFloat>(1, std::round(size.height * scale)));
    if (!CGSizeEqualToSize(metal.drawableSize, px)) metal.drawableSize = px;
    size_t w = CVPixelBufferGetWidth(image), h = CVPixelBufferGetHeight(image);
    CVMetalTextureRef ref = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(nullptr, cache, image, nullptr, MTLPixelFormatBGRA8Unorm, w, h, 0, &ref) != kCVReturnSuccess || !ref) return false;
    id<MTLTexture> tex = CVMetalTextureGetTexture(ref);
    id<CAMetalDrawable> drawable = tex ? [metal nextDrawable] : nil;
    if (!drawable) {
      CFRelease(ref);
      return false;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [commands commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    // letterbox on whole pixels (at 1:1 each pixel the picture's own)
    double k = std::min(px.width / double(w), px.height / double(h));
    double dw = std::max(1.0, std::round(w * k)), dh = std::max(1.0, std::round(h * k));
    double x0 = std::floor((px.width - dw) / 2), y0 = std::floor((px.height - dh) / 2);
    float rect[4] = {float(x0 / px.width), float(y0 / px.height), float((x0 + dw) / px.width), float((y0 + dh) / px.height)};
    if (firstAt == 0) firstAt = nowMs();
    float t = float(std::min(1.0, (nowMs() - firstAt) / 220.0));
    float look[4] = {1 - (1 - t) * (1 - t), 0, float(w / dw), float(h / dh)};
    [enc setRenderPipelineState:pipeline];
    [enc setVertexBytes:rect length:sizeof rect atIndex:0];
    [enc setFragmentTexture:tex atIndex:0];
    [enc setFragmentBytes:look length:sizeof look atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
    [cb presentDrawable:drawable];
    CVMetalTextureRef keep = ref; // (the texture keeps the frame alive until the GPU is done)
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      CFRelease(keep);
    }];
    [cb commit];
    return true;
  }

  /** The fade-in goes on even if the picture stands still. */
  void fadeTick() {
    if (!metal || !shown || nowMs() - firstAt > 260) return;
    drawMetal(shown);
    std::weak_ptr<Impl> weak = shared_from_this();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC), queue, ^{
      auto s = weak.lock();
      if (s) s->fadeTick();
    });
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
    bool first = firstAt == 0;
    if (!drawMetal(image)) {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      layer.contents = (__bridge id)surface;
      [CATransaction commit];
      [CATransaction flush];
    }
    if (shown) CVPixelBufferRelease(shown);
    shown = image;
    if (first && metal) fadeTick();
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
  layer.contents = nil;
  impl_->makeMetal(layer);
}

MacPlayer::~MacPlayer() {
  detach();
  [impl_->metal removeFromSuperlayer];
}

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
  st.metal = impl_->pipeline != nil;
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
