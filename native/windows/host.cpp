#include "host.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

#include "audio.h"
#include "capture.h"
#include "clock.h"
#include "encoder.h"

namespace ss {

struct HostPipeline::Impl {
  Engine& engine;
  Options opts;
  std::thread thread;
  std::atomic<bool> quit{false}, streaming{false}, wantKey{true};
  std::atomic<double> bitrate{20e6};
  mutable std::mutex mu;
  Info info;
  std::unique_ptr<AudioCapture> audio;

  explicit Impl(Engine& e) : engine(e) {}

  void setInfo(const std::function<void(Info&)>& fn) {
    std::lock_guard<std::mutex> l(mu);
    fn(info);
  }

  /** The encoder for this capture: NVENC on NVIDIA, the GPU's own, or software. */
  std::unique_ptr<VideoEncoder> openEncoder(ScreenCapture& cap, std::string& why) {
    int bps = int(bitrate.load());
    if (cap.nvidia()) {
      auto nv = createNvencEncoder();
      why = nv->open(cap.device(), cap.context(), cap.frame(), cap.width(), cap.height(), opts.fps, bps);
      if (why.empty()) return nv;
    }
    auto hw = createMfEncoder(cap.adapterLuid(), true);
    why = hw->open(cap.device(), cap.context(), cap.frame(), cap.width(), cap.height(), opts.fps, bps);
    if (why.empty()) return hw;
    return nullptr;
  }

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    ScreenCapture cap;
    std::unique_ptr<VideoEncoder> enc;
    int failures = 0;
    bool software = false;
    double lastBitrate = 0;
    while (!quit) {
      // (re)open: at the start, after a display mode change, a UAC prompt, a full-screen game …
      int maxW = software ? std::min(opts.maxWidth, 1920) : opts.maxWidth;
      int maxH = software ? std::min(opts.maxHeight, 1080) : opts.maxHeight;
      int fps = software ? std::min(opts.fps, 30) : opts.fps;
      std::string why = cap.open(opts.monitor, maxW, maxH);
      if (!why.empty()) {
        setInfo([&](Info& i) { i.error = why; });
        if (++failures > 50) break;
        Sleep(200);
        continue;
      }
      enc = software ? nullptr : openEncoder(cap, why);
      if (!enc) {
        // no GPU encoder: Windows' software encoder, at most 1080p 30
        if (!software) {
          software = true;
          continue;
        }
        enc = createMfEncoder(cap.adapterLuid(), false);
        why = enc->open(cap.device(), cap.context(), cap.frame(), cap.width(), cap.height(), fps, int(bitrate.load()));
        if (!why.empty()) {
          setInfo([&](Info& i) { i.error = "encoder: " + why; });
          break;
        }
      }
      failures = 0;
      std::string name = enc->name();
      engine.setEncoder(name, enc->hardware());
      setInfo([&](Info& i) {
        i.encoder = name;
        i.hardware = enc->hardware();
        i.width = cap.width();
        i.height = cap.height();
        i.error.clear();
      });
      wantKey = true;
      lastBitrate = bitrate;
      double interval = 1000.0 / fps, lastEncode = -1e9, captureMs = nowMs(), encSum = 0, statsAt = nowMs();
      // The frame rate: a new image is encoded the moment it comes if its turn is near (within a
      // quarter frame), and the turns are one interval apart on average – counted from when the
      // last one was due, not from when its encoding ended, which held back an image that came in
      // time (a 60 Hz screen at 60 FPS waited part of a frame, and every few frames one was lost).
      const double slack = interval / 4;
      double nextAt = -1e9;
      int encoded = 0;
      bool pending = false, have = false;
      uint64_t frameNo = 0;
      bool lost = false;
      while (!quit && !lost) {
        if (std::fabs(bitrate - lastBitrate) > 1) {
          lastBitrate = bitrate;
          enc->setBitrate(int(lastBitrate));
        }
        // waiting for the next image; with one pending (and someone to send it to), only until its turn
        unsigned timeout = pending && streaming ? unsigned(std::max(0.0, std::ceil(nextAt - slack - nowMs()))) : 50;
        double t = 0;
        int r = cap.acquire(timeout, t);
        if (r < 0) {
          lost = true;
          break;
        }
        if (r > 0) {
          captureMs = t;
          have = true;
          pending = true;
        }
        // a keyframe asked for while the screen stands still: the last image again
        if (have && !pending && wantKey && nowMs() - lastEncode >= interval) pending = true;
        if (!pending || !streaming || nowMs() < nextAt - slack) continue;
        double started = nowMs();
        nextAt = std::max(nextAt, started) + interval; // (late, or after a still screen: from now)
        cap.render();
        bool key = wantKey.exchange(false);
        bool ok = enc->encode(key, captureMs, [&](const uint8_t* d, size_t n, bool isKey, double capture) {
          engine.sendVideo(d, n, isKey, capture, double(frameNo) * 1e6 / fps);
        });
        frameNo++;
        lastEncode = nowMs();
        pending = false;
        if (!ok) {
          lost = true; // a broken encoder: open everything again
          break;
        }
        encSum += lastEncode - started;
        encoded++;
        if (lastEncode - statsAt >= 1000) {
          double ms = encSum / std::max(1, encoded);
          double rate = encoded * 1000.0 / (lastEncode - statsAt);
          engine.setEncoderStats(ms, cap.width(), cap.height());
          setInfo([&](Info& i) {
            i.encodeMs = ms;
            i.fps = rate;
          });
          encSum = 0;
          encoded = 0;
          statsAt = lastEncode;
        }
      }
      enc.reset();
      cap.close();
      if (!quit) Sleep(50);
    }
    enc.reset();
    cap.close();
    CoUninitialize();
  }
};

HostPipeline::HostPipeline(Engine& engine) : impl_(new Impl(engine)) {}
HostPipeline::~HostPipeline() { stop(); }

void HostPipeline::start(const Options& options) {
  stop();
  impl_->opts = options;
  impl_->quit = false;
  impl_->info = Info{};
  impl_->thread = std::thread([this] { impl_->run(); });
  if (options.audio) {
    Engine* engine = &impl_->engine;
    impl_->audio.reset(new AudioCapture([engine](int pair, int channels, const uint8_t* d, size_t n, double ts) { engine->sendAudio(pair, channels, d, n, ts); }));
    std::string why = impl_->audio->start();
    std::string layout = why.empty() ? impl_->audio->layout() : "";
    impl_->setInfo([&](Info& i) {
      i.audioLayout = layout;
      i.audioError = why;
    });
    if (!why.empty()) impl_->audio.reset();
  }
}

void HostPipeline::stop() {
  impl_->quit = true;
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->audio.reset();
}

void HostPipeline::setStreaming(bool on) {
  impl_->streaming = on;
  if (on) impl_->wantKey = true;
}

void HostPipeline::keyframe() { impl_->wantKey = true; }

void HostPipeline::setBitrate(double bps) { impl_->bitrate = bps; }

HostPipeline::Info HostPipeline::info() const {
  std::lock_guard<std::mutex> l(impl_->mu);
  return impl_->info;
}

}  // namespace ss
