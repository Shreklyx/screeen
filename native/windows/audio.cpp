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

#include "audio_ring.h"
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

/** Says when Windows' default output changes (headphones plugged in, another device chosen, a
 *  monitor's speakers gone to sleep): the capture and the playback then open the new one. */
class DeviceWatch : public IMMNotificationClient {
 public:
  std::atomic<bool> changed{false};
  // (lives as long as its owner, which unregisters it first: no counting)
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) changed = true;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

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
    ComPtr<IMMDeviceEnumerator> en;
    DeviceWatch watch;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) en->RegisterEndpointNotificationCallback(&watch);
    // the default output's sound, until stopped – when that device changes or goes away, the new
    // one's (the viewers would hear nothing more otherwise)
    bool first = true;
    while (!quit) {
      watch.changed = false;
      std::string why = en ? loop(en.Get(), watch) : "no-device";
      if (first) {
        first = false;
        if (!why.empty()) { // (the first try's failure is what start() reports)
          error = why;
          started = -1;
          break;
        }
      }
      if (!quit) Sleep(why.empty() ? 30 : 500);
    }
    if (en) en->UnregisterEndpointNotificationCallback(&watch);
    CoUninitialize();
  }

  /** One device's sound: returns when stopped, when the default device changed or this one went
   *  away ("" – open the default one again), or with what failed. */
  std::string loop(IMMDeviceEnumerator* en, DeviceWatch& watch) {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
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
    bool lost = false;
    while (!quit && !watch.changed && !lost) {
      if (WaitForSingleObject(event, 50) != WAIT_OBJECT_0) {
        // nothing plays: Windows sends no packets – the next sound starts a fresh timeline
        continue;
      }
      for (;;) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpcPos = 0;
        HRESULT got = capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPos);
        if (got == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(got)) { // the device went away (unplugged, gone to sleep): the default one again
          lost = true;
          break;
        }
        if (streamUs < 0 || pending.empty()) {
          // the timestamp: when this packet was captured (QPC, 100 ns) on the engine's clock
          LARGE_INTEGER now;
          QueryPerformanceCounter(&now);
          LARGE_INTEGER f;
          QueryPerformanceFrequency(&f);
          double ageMs = (double(now.QuadPart) / f.QuadPart * 1e7 - double(qpcPos)) / 1e4;
          // (no time – the driver says so – or one it cannot have: now; a wrong one put the sound
          // seconds behind, where the viewers dropped it)
          if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !(ageMs >= 0 && ageMs < 1000)) ageMs = 0;
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

/* ------------------------------------------------ the viewer: Opus → ring (audio_ring.cpp) → speakers */

struct AudioPlayer::Impl {
  AudioRing ring;
  std::thread thread;
  std::atomic<bool> quit{false};

  void pull(float* out, int outs, int n) { ring.pull(out, outs, n); }

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    DeviceWatch watch;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) en->RegisterEndpointNotificationCallback(&watch);
    while (!quit && en) {
      watch.changed = false;
      // returns when the device goes away or another becomes the default: that one then
      bool played = play(en.Get(), watch);
      if (!quit) Sleep(played && watch.changed ? 30 : 500);
    }
    if (en) en->UnregisterEndpointNotificationCallback(&watch);
    CoUninitialize();
  }

  bool play(IMMDeviceEnumerator* en, DeviceWatch& watch) {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
      return false;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix))) return false;
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
      return false;
    }
    client->SetEventHandle(event);
    ComPtr<IAudioRenderClient> render;
    client->GetService(IID_PPV_ARGS(&render));
    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    client->Start();
    while (!quit && !watch.changed) {
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
    return true;
  }
};

AudioPlayer::AudioPlayer() : impl_(new Impl) {
  impl_->thread = std::thread([this] { impl_->run(); });
}

AudioPlayer::~AudioPlayer() {
  impl_->quit = true;
  if (impl_->thread.joinable()) impl_->thread.join();
}

void AudioPlayer::push(int sub, const uint8_t* data, size_t n, double tsUs) { impl_->ring.push(sub, data, n, tsUs); }

void AudioPlayer::setVolume(float volume) { impl_->ring.setVolume(volume); }

void AudioPlayer::reset() { impl_->ring.reset(); }

}  // namespace ss
