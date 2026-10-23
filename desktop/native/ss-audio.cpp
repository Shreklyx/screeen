// ss-audio – records what Windows is playing, with every channel of the output
// device (stereo, 5.1 or 7.1), for the Screen Share desktop app.
//
// Chromium's own system-audio capture is stereo only; this helper is what makes
// 5.1/7.1 sharing possible. It uses WASAPI loopback on the default output device
// and runs its capture loop at "Pro Audio" priority (MMCSS).
//
//   ss-audio.exe           capture until stdin is closed (the app quits or stops it)
//   ss-audio.exe --probe   print the output device's format as JSON and exit
//   ss-audio.exe --input   record the default input device instead (testing, e.g. under Wine)
//
// stdout carries packets: 4-byte tag, uint32 little-endian payload size, payload
//   "FMT1"  uint32 sampleRate, uint32 channels, uint32 channelMask (WAVEFORMATEXTENSIBLE)
//   "PCM1"  float32 samples, interleaved, `channels` per frame
//   "ERR1"  UTF-8 text (no output device, …) – capture is retried automatically
// A new FMT1 follows whenever the default output device or its format changes.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// The GUIDs are spelled out so the file builds the same with MSVC and MinGW.
static const CLSID kMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const IID kIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const IID kIAudioSessionControl = {0xF4B1A599, 0x7266, 0x4319, {0xA8, 0xCA, 0xE7, 0x0A, 0xCB, 0x11, 0xE8, 0xCD}};
static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static const REFERENCE_TIME kBufferDuration = 1000000;  // 100 ms of room; latency comes from the polling below
static const DWORD kPollMs = 2;
static const DWORD kDeviceCheckMs = 500;

static HANDLE g_out = INVALID_HANDLE_VALUE;
static volatile LONG g_quit = 0;
static bool g_fromInput = false;

template <class T>
static void release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

/* ------------------------------------------------------------------ output */

static bool writeAll(const void* data, DWORD size) {
  const char* p = static_cast<const char*>(data);
  while (size) {
    DWORD n = 0;
    if (!WriteFile(g_out, p, size, &n, nullptr) || n == 0) return false;
    p += n;
    size -= n;
  }
  return true;
}

// A failed write means the app is gone: stop.
static void sendPacket(const char tag[4], const void* payload, uint32_t size) {
  unsigned char head[8];
  memcpy(head, tag, 4);
  head[4] = size & 0xFF;
  head[5] = (size >> 8) & 0xFF;
  head[6] = (size >> 16) & 0xFF;
  head[7] = (size >> 24) & 0xFF;
  if (!writeAll(head, 8) || (size && !writeAll(payload, size))) ExitProcess(0);
}

static void sendError(const char* text) { sendPacket("ERR1", text, static_cast<uint32_t>(strlen(text))); }

// stdin closed (the app quit or stopped the capture) → stop.
static DWORD WINAPI watchStdin(LPVOID) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  char buf[64];
  DWORD n = 0;
  while (ReadFile(in, buf, sizeof buf, &n, nullptr) && n > 0) {
  }
  InterlockedExchange(&g_quit, 1);
  return 0;
}

/* ------------------------------------------------------------------ formats */

enum SampleType { kF32, kI16, kI24, kI32 };

struct Format {
  uint32_t rate = 0;
  uint32_t channels = 0;
  uint32_t mask = 0;
  uint32_t blockAlign = 0;
  SampleType type = kF32;
};

static uint32_t defaultMask(uint32_t channels) {
  switch (channels) {
    case 1: return 0x4;      // centre
    case 2: return 0x3;      // L R
    case 4: return 0x33;     // L R BL BR
    case 6: return 0x60F;    // 5.1: L R C LFE SL SR
    case 8: return 0x63F;    // 7.1: L R C LFE BL BR SL SR
    default: return 0;
  }
}

static bool describe(const WAVEFORMATEX* wf, Format& f) {
  f.rate = wf->nSamplesPerSec;
  f.channels = wf->nChannels;
  f.blockAlign = wf->nBlockAlign;
  f.mask = 0;
  WORD tag = wf->wFormatTag;
  if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
    const WAVEFORMATEXTENSIBLE* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    f.mask = x->dwChannelMask;
    tag = IsEqualGUID(x->SubFormat, kSubtypeFloat) ? WAVE_FORMAT_IEEE_FLOAT : IsEqualGUID(x->SubFormat, kSubtypePcm) ? WAVE_FORMAT_PCM : 0;
  }
  const WORD bits = wf->wBitsPerSample;
  if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) f.type = kF32;
  else if (tag == WAVE_FORMAT_PCM && bits == 16) f.type = kI16;
  else if (tag == WAVE_FORMAT_PCM && bits == 24) f.type = kI24;
  else if (tag == WAVE_FORMAT_PCM && bits == 32) f.type = kI32;
  else return false;
  if (!f.mask) f.mask = defaultMask(f.channels);
  return f.channels > 0 && f.rate > 0 && f.blockAlign == f.channels * (bits / 8);
}

static void appendSamples(std::vector<float>& out, const BYTE* data, UINT32 frames, const Format& f, bool silent) {
  const size_t count = static_cast<size_t>(frames) * f.channels;
  const size_t start = out.size();
  out.resize(start + count, 0.0f);
  if (silent || !data) return;
  float* dst = out.data() + start;
  switch (f.type) {
    case kF32:
      memcpy(dst, data, count * sizeof(float));
      break;
    case kI16:
      for (size_t i = 0; i < count; i++) {
        int16_t s;
        memcpy(&s, data + i * 2, 2);
        dst[i] = s / 32768.0f;
      }
      break;
    case kI24:
      for (size_t i = 0; i < count; i++) {
        const BYTE* p = data + i * 3;
        int32_t s = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 24)) >> 8;
        dst[i] = s / 8388608.0f;
      }
      break;
    case kI32:
      for (size_t i = 0; i < count; i++) {
        int32_t s;
        memcpy(&s, data + i * 4, 4);
        dst[i] = static_cast<float>(s / 2147483648.0);
      }
      break;
  }
}

/* ------------------------------------------------------------------ devices */

static std::wstring defaultDeviceId(IMMDeviceEnumerator* en, IMMDevice** device) {
  IMMDevice* dev = nullptr;
  std::wstring id;
  if (SUCCEEDED(en->GetDefaultAudioEndpoint(g_fromInput ? eCapture : eRender, eConsole, &dev))) {
    LPWSTR raw = nullptr;
    if (SUCCEEDED(dev->GetId(&raw)) && raw) {
      id = raw;
      CoTaskMemFree(raw);
    }
  }
  if (device) *device = dev;
  else release(dev);
  return id;
}

static int probe(IMMDeviceEnumerator* en) {
  IMMDevice* dev = nullptr;
  defaultDeviceId(en, &dev);
  IAudioClient* client = nullptr;
  WAVEFORMATEX* mix = nullptr;
  Format f;
  bool ok = dev && SUCCEEDED(dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
            SUCCEEDED(client->GetMixFormat(&mix)) && describe(mix, f);
  if (ok) printf("{\"rate\":%u,\"channels\":%u,\"mask\":%u}\n", f.rate, f.channels, f.mask);
  else printf("{\"error\":\"%s\"}\n", dev ? "unsupported-format" : "no-device");
  if (mix) CoTaskMemFree(mix);
  release(client);
  release(dev);
  return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ capture */

// Loopback capture delivers nothing while nothing plays; a silent stream on the
// same device keeps the engine running, so the app gets a continuous signal.
struct SilentPlayer {
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  UINT32 frames = 0;

  void start(IMMDevice* dev, const WAVEFORMATEX* fmt) {
    if (FAILED(dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
        FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, fmt, nullptr)) ||
        FAILED(client->GetBufferSize(&frames)) ||
        FAILED(client->GetService(kIAudioRenderClient, reinterpret_cast<void**>(&render)))) {
      stop();
      return;
    }
    IAudioSessionControl* session = nullptr;
    if (SUCCEEDED(client->GetService(kIAudioSessionControl, reinterpret_cast<void**>(&session)))) {
      session->SetDisplayName(L"Screen Share", nullptr);
      release(session);
    }
    feed();
    client->Start();
  }

  void feed() {
    if (!render) return;
    UINT32 padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding)) || padding >= frames) return;
    BYTE* data = nullptr;
    const UINT32 n = frames - padding;
    if (SUCCEEDED(render->GetBuffer(n, &data))) render->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
  }

  void stop() {
    if (client) client->Stop();
    release(render);
    release(client);
    frames = 0;
  }
};

// Opens the default output device's loopback stream; returns when the device
// changes, disappears or fails (the caller opens it again) or when told to quit.
static void captureSession(IMMDeviceEnumerator* en) {
  IMMDevice* dev = nullptr;
  const std::wstring id = defaultDeviceId(en, &dev);
  if (!dev) {
    sendError("no-device");
    for (int i = 0; i < 10 && !g_quit; i++) Sleep(100);
    return;
  }

  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  WAVEFORMATEX* mix = nullptr;
  SilentPlayer silence;
  Format f;
  const char* step = "activate";
  HRESULT hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  if (SUCCEEDED(hr)) {
    step = "mix-format";
    hr = client->GetMixFormat(&mix);
  }
  if (SUCCEEDED(hr)) {
    // 48 kHz float with the device's own channel layout (Windows converts when the
    // device runs at another rate); the device format itself as a fallback.
    WAVEFORMATEXTENSIBLE want = {};
    want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    want.Format.nChannels = mix->nChannels;
    want.Format.nSamplesPerSec = 48000;
    want.Format.wBitsPerSample = 32;
    want.Format.nBlockAlign = static_cast<WORD>(mix->nChannels * 4);
    want.Format.nAvgBytesPerSec = 48000 * want.Format.nBlockAlign;
    want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    want.Samples.wValidBitsPerSample = 32;
    Format mixInfo;
    want.dwChannelMask = describe(mix, mixInfo) ? mixInfo.mask : defaultMask(mix->nChannels);
    want.SubFormat = kSubtypeFloat;
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    step = "initialize";
    const DWORD loopback = g_fromInput ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback | convert, kBufferDuration, 0, &want.Format, nullptr);
    if (SUCCEEDED(hr)) {
      describe(&want.Format, f);
    } else {
      release(client);  // a client whose Initialize failed cannot be initialized again
      step = "initialize-native";
      hr = dev->Activate(kIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
      if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback, kBufferDuration, 0, mix, nullptr);
      if (SUCCEEDED(hr) && !describe(mix, f)) {
        step = "unsupported-format";
        hr = E_FAIL;
      }
    }
  }
  if (SUCCEEDED(hr)) {
    step = "capture-client";
    hr = client->GetService(kIAudioCaptureClient, reinterpret_cast<void**>(&capture));
  }
  if (SUCCEEDED(hr)) {
    if (!g_fromInput) silence.start(dev, mix);
    step = "start";
    hr = client->Start();
  }

  if (FAILED(hr)) {
    char text[96];
    snprintf(text, sizeof text, "open-failed: %s 0x%08lX", step, static_cast<unsigned long>(hr));
    sendError(text);
    for (int i = 0; i < 10 && !g_quit; i++) Sleep(100);
  } else {
    const uint32_t info[3] = {f.rate, f.channels, f.mask};
    sendPacket("FMT1", info, sizeof info);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(f.rate / 50) * f.channels);
    DWORD lastCheck = GetTickCount();
    bool running = true;
    while (running && !g_quit) {
      silence.feed();
      UINT32 packet = 0;
      hr = capture->GetNextPacketSize(&packet);
      while (SUCCEEDED(hr) && packet) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;
        appendSamples(out, data, frames, f, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
        capture->ReleaseBuffer(frames);
        hr = capture->GetNextPacketSize(&packet);
      }
      if (FAILED(hr)) running = false;  // device unplugged / format changed → reopen
      if (!out.empty()) {
        sendPacket("PCM1", out.data(), static_cast<uint32_t>(out.size() * sizeof(float)));
        out.clear();
      }
      if (GetTickCount() - lastCheck >= kDeviceCheckMs) {
        lastCheck = GetTickCount();
        if (defaultDeviceId(en, nullptr) != id) running = false;  // another default output device
      }
      if (running) Sleep(kPollMs);
    }
    client->Stop();
  }

  silence.stop();
  release(capture);
  release(client);
  if (mix) CoTaskMemFree(mix);
  release(dev);
}

int main(int argc, char** argv) {
  const bool probeOnly = argc > 1 && strcmp(argv[1], "--probe") == 0;
  g_fromInput = argc > 1 && strcmp(argv[1], "--input") == 0;
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIMMDeviceEnumerator, reinterpret_cast<void**>(&en)))) {
    if (probeOnly) printf("{\"error\":\"no-audio-service\"}\n");
    else sendError("no-audio-service");
    return 3;
  }
  if (probeOnly) return probe(en);

  HANDLE watcher = CreateThread(nullptr, 0, watchStdin, nullptr, 0, nullptr);
  if (watcher) CloseHandle(watcher);
  timeBeginPeriod(1);  // Sleep(2) really sleeps ~2 ms
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
  while (!g_quit) captureSession(en);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  timeEndPeriod(1);
  release(en);
  CoUninitialize();
  return 0;
}
