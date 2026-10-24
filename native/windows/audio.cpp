#include "audio.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <opus.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "clock.h"

namespace ss {

using Microsoft::WRL::ComPtr;

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (not linked in every toolchain)
const GUID kFloatFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

const int kRate = 48000;
const int kFrame = 120; // 2.5 ms: the least delay Opus has

/** Samples of the device's mix format as float (most devices already are). */
float sampleAt(const BYTE* p, const WAVEFORMATEX* f, size_t frame, int channel) {
  bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                 (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == kFloatFormat);
  size_t i = frame * f->nChannels + size_t(channel);
  if (isFloat && f->wBitsPerSample == 32) return reinterpret_cast<const float*>(p)[i];
  if (f->wBitsPerSample == 16) return reinterpret_cast<const int16_t*>(p)[i] / 32768.0f;
  if (f->wBitsPerSample == 32) return float(reinterpret_cast<const int32_t*>(p)[i] / 2147483648.0);
  if (f->wBitsPerSample == 24) {
    const BYTE* s = p + i * 3;
    int32_t v = int32_t(uint32_t(s[0]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 24);
    return float(v / 2147483648.0);
  }
  return 0;
}

}  // namespace

/* ------------------------------------------------ the sharer: loopback → Opus */

struct AudioCapture::Impl {
  Packet packet;
  std::thread thread;
  std::atomic<bool> quit{false};
  std::string layoutName = "sztereó";
  std::string error;
  std::atomic<int> started{0};

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    error = loop();
    started = error.empty() ? 1 : -1;
    CoUninitialize();
  }

  std::string loop() {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
      return "no-device";
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix))) return "no-format";
    int channels = std::min<int>(8, mix->nChannels);
    int deviceRate = int(mix->nSamplesPerSec);
    layoutName = channels >= 8 ? "7.1" : channels >= 6 ? "5.1" : "sztereó";
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, mix, nullptr);
    if (FAILED(hr)) {
      CoTaskMemFree(mix);
      CloseHandle(event);
      return "loopback";
    }
    client->SetEventHandle(event);
    ComPtr<IAudioCaptureClient> capture;
    client->GetService(IID_PPV_ARGS(&capture));
    // one Opus encoder per channel pair; 5 ms packets, low delay
    int pairs = (channels + 1) / 2;
    std::vector<OpusEncoder*> encoders;
    for (int p = 0; p < pairs; p++) {
      int ch = std::min(2, channels - p * 2);
      int err = 0;
      OpusEncoder* e = opus_encoder_create(kRate, ch, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
      opus_encoder_ctl(e, OPUS_SET_BITRATE(ch * 128000));
      encoders.push_back(e);
    }
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    client->Start();
    started = 1;
    // resampled to 48 kHz (linear: loopback is usually 48 kHz already) and cut into 5 ms frames
    std::vector<float> pending; // interleaved, `channels` wide, at 48 kHz
    double pos = 0;             // position in the device's samples (resampling)
    double streamUs = -1;       // timestamp of pending[0]
    uint8_t out[1500];
    std::vector<float> pairBuf(kFrame * 2);
    while (!quit) {
      if (WaitForSingleObject(event, 50) != WAIT_OBJECT_0) {
        // nothing plays: Windows sends no packets – the next sound starts a fresh timeline
        continue;
      }
      for (;;) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpcPos = 0;
        if (capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPos) != S_OK) break;
        if (streamUs < 0 || pending.empty()) {
          // the timestamp: when this packet was captured (QPC, 100 ns) on the engine's clock
          LARGE_INTEGER now;
          QueryPerformanceCounter(&now);
          LARGE_INTEGER f;
          QueryPerformanceFrequency(&f);
          double ageMs = (double(now.QuadPart) / f.QuadPart * 1e7 - double(qpcPos)) / 1e4;
          streamUs = (nowMs() - ageMs) * 1000;
        }
        bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        double step = double(deviceRate) / kRate;
        // linear resampling from the device rate to 48 kHz
        for (; pos < frames; pos += step) {
          size_t i0 = size_t(pos);
          double fr = pos - double(i0);
          for (int c = 0; c < channels; c++) {
            float a = silent ? 0 : sampleAt(data, mix, i0, c);
            float b = silent ? 0 : sampleAt(data, mix, std::min<size_t>(i0 + 1, frames - 1), c);
            pending.push_back(float(a + (b - a) * fr));
          }
        }
        pos -= frames;
        capture->ReleaseBuffer(frames);
        while (pending.size() >= size_t(kFrame * channels)) {
          for (int p = 0; p < pairs; p++) {
            int ch = std::min(2, channels - p * 2);
            for (int i = 0; i < kFrame; i++)
              for (int c = 0; c < ch; c++) pairBuf[size_t(i * ch + c)] = pending[size_t(i * channels + p * 2 + c)];
            int n = opus_encode_float(encoders[size_t(p)], pairBuf.data(), kFrame, out, sizeof out);
            if (n > 0) packet(p, channels, out, size_t(n), streamUs);
          }
          pending.erase(pending.begin(), pending.begin() + kFrame * channels);
          streamUs += kFrame * 1e6 / kRate;
        }
      }
    }
    client->Stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    for (OpusEncoder* e : encoders) opus_encoder_destroy(e);
    CoTaskMemFree(mix);
    CloseHandle(event);
    return "";
  }
};

AudioCapture::AudioCapture(Packet packet) : impl_(new Impl) { impl_->packet = std::move(packet); }
AudioCapture::~AudioCapture() { stop(); }

std::string AudioCapture::start() {
  stop();
  impl_->quit = false;
  impl_->started = 0;
  impl_->thread = std::thread([this] { impl_->run(); });
  for (int i = 0; i < 300 && impl_->started == 0; i++) Sleep(10);
  return impl_->started < 0 ? impl_->error : "";
}

void AudioCapture::stop() {
  impl_->quit = true;
  if (impl_->thread.joinable()) impl_->thread.join();
}

std::string AudioCapture::layout() const { return impl_->layoutName; }

/* ------------------------------------------------ the viewer: Opus → ring → speakers */

namespace {
const int kRing = kRate; // one second
const int kMinTarget = 240, kMaxTarget = 4800; // 5 … 100 ms
const float K = 0.70710678f;
float soft(float x) { return x > 0.9f ? 0.9f + 0.1f * std::tanh((x - 0.9f) / 0.1f) : x < -0.9f ? -0.9f + 0.1f * std::tanh((x + 0.9f) / 0.1f) : x; }
}  // namespace

struct AudioPlayer::Impl {
  std::mutex mu;
  int channels = 0;             // of the stream
  std::vector<std::vector<float>> ring;
  std::vector<OpusDecoder*> decoders;
  long long read = LLONG_MIN;   // absolute sample index of the next output sample
  long long end = LLONG_MIN;    // one past the newest written sample
  int target = 480;             // 10 ms to start with
  long long calm = 0;
  std::atomic<float> volume{1.0f};
  std::thread thread;
  std::atomic<bool> quit{false};

  ~Impl() { for (OpusDecoder* d : decoders) if (d) opus_decoder_destroy(d); }

  void configure(int ch) {
    for (OpusDecoder* d : decoders) if (d) opus_decoder_destroy(d);
    decoders.assign(size_t((ch + 1) / 2), nullptr);
    channels = ch;
    ring.assign(size_t(ch), std::vector<float>(kRing, 0.0f));
    read = end = LLONG_MIN;
    target = 480;
  }

  void push(int sub, const uint8_t* data, size_t n, double tsUs) {
    int ch = std::max(1, std::min(8, sub >> 4));
    int pair = sub & 15;
    if (pair * 2 >= ch) return;
    std::lock_guard<std::mutex> l(mu);
    if (ch != channels) configure(ch);
    int pc = std::min(2, ch - pair * 2);
    OpusDecoder*& dec = decoders[size_t(pair)];
    if (!dec) {
      int err = 0;
      dec = opus_decoder_create(kRate, pc, &err);
      if (!dec) return;
    }
    float pcm[5760 * 2];
    int frames = opus_decode_float(dec, data, opus_int32(n), pcm, 5760, 0);
    if (frames <= 0) return;
    long long at = llround(tsUs * kRate / 1e6);
    if (read == LLONG_MIN) read = at - target;
    if (at + frames <= read) return; // too late, already played
    if (at - read > kRing - 2048) read = at - target; // far ahead (clock jump): start over
    for (int c = 0; c < pc; c++) {
      std::vector<float>& r = ring[size_t(pair * 2 + c)];
      for (int i = 0; i < frames; i++) {
        long long a = at + i;
        if (a >= read) r[size_t(((a % kRing) + kRing) % kRing)] = pcm[i * pc + c];
      }
    }
    if (at + frames > end) end = at + frames;
  }

  /** Fills `out` (outs channels, n frames, interleaved float) – the device thread. */
  void pull(float* out, int outs, int n) {
    std::fill(out, out + size_t(n) * outs, 0.0f);
    std::lock_guard<std::mutex> l(mu);
    if (read == LLONG_MIN || !channels) return;
    long long buffered = end - read;
    if (buffered < n) {
      // ran dry: wait a little longer next time
      target = std::min(kMaxTarget, target + 240);
      calm = 0;
      if (buffered <= 0) read = end - target;
      return;
    }
    if (buffered > target + 2400) read = end - target; // the buffer grew (clock drift): catch up
    calm += n;
    if (calm > kRate * 5 && target > kMinTarget) {
      target -= 48;
      calm = 0;
    }
    float v = volume;
    int ch = channels;
    for (int i = 0; i < n; i++) {
      size_t idx = size_t((((read + i) % kRing) + kRing) % kRing);
      auto s = [&](int c) { return c < ch ? ring[size_t(c)][idx] : 0.0f; };
      float* o = out + size_t(i) * outs;
      if (ch == 1) {
        for (int k = 0; k < std::min(outs, 2); k++) o[k] = s(0);
      } else if (ch <= 2 || outs >= ch) {
        for (int k = 0; k < std::min(outs, ch); k++) o[k] = s(k);
      } else if (outs >= 6) {
        // 7.1 → 5.1: sides folded into the surrounds
        o[0] = s(0); o[1] = s(1); o[2] = s(2); o[3] = s(3);
        o[4] = soft(K * (s(4) + s(6)));
        o[5] = soft(K * (s(5) + s(7)));
      } else {
        o[0] = soft(s(0) + K * s(2) + K * s(4) + K * s(6));
        if (outs > 1) o[1] = soft(s(1) + K * s(2) + K * s(5) + K * s(7));
      }
      for (int k = 0; k < outs; k++) o[k] *= v;
      for (int c = 0; c < ch; c++) ring[size_t(c)][idx] = 0; // never play a sample twice
    }
    read += n;
  }

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!quit) {
      play(); // returns when the device goes away: open the (new) default one
      if (!quit) Sleep(500);
    }
    CoUninitialize();
  }

  void play() {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
      return;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix))) return;
    // our own format: float at 48 kHz with the device's channels (Windows converts if needed)
    WAVEFORMATEXTENSIBLE fmt = {};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = mix->nChannels;
    fmt.Format.nSamplesPerSec = kRate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = WORD(4 * mix->nChannels);
    fmt.Format.nAvgBytesPerSec = fmt.Format.nBlockAlign * kRate;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask : (mix->nChannels == 2 ? 3 : 0);
    fmt.SubFormat = kFloatFormat;
    int outs = mix->nChannels;
    CoTaskMemFree(mix);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 100000, 0,
                                    &fmt.Format, nullptr);
    if (FAILED(hr)) {
      CloseHandle(event);
      return;
    }
    client->SetEventHandle(event);
    ComPtr<IAudioRenderClient> render;
    client->GetService(IID_PPV_ARGS(&render));
    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    client->Start();
    while (!quit) {
      if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
      UINT32 padding = 0;
      if (FAILED(client->GetCurrentPadding(&padding))) break; // the device went away
      // keep the device's own buffer short: about 10 ms queued
      UINT32 want = std::min<UINT32>(bufferFrames - padding, padding < 480 ? 480 - padding : 0);
      if (!want) continue;
      BYTE* data = nullptr;
      if (FAILED(render->GetBuffer(want, &data))) break;
      pull(reinterpret_cast<float*>(data), outs, int(want));
      render->ReleaseBuffer(want, 0);
    }
    client->Stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CloseHandle(event);
  }
};

AudioPlayer::AudioPlayer() : impl_(new Impl) {
  impl_->thread = std::thread([this] { impl_->run(); });
}

AudioPlayer::~AudioPlayer() {
  impl_->quit = true;
  if (impl_->thread.joinable()) impl_->thread.join();
}

void AudioPlayer::push(int sub, const uint8_t* data, size_t n, double tsUs) { impl_->push(sub, data, n, tsUs); }

void AudioPlayer::setVolume(float volume) { impl_->volume = std::max(0.0f, std::min(1.0f, volume)); }

void AudioPlayer::reset() {
  std::lock_guard<std::mutex> l(impl_->mu);
  impl_->configure(std::max(1, impl_->channels));
}

}  // namespace ss
