// ss-capture – native screen capture and NVIDIA hardware H.264 encoding for direct mode
// (desktop app → desktop app), the shortest path from the screen to the network:
//
//  - DXGI Desktop Duplication hands over a new desktop image the moment Windows composes it
//    (Chromium's capturer asks at a fixed rate, on average half a frame later),
//  - the image stays on the GPU: copied, the mouse pointer drawn in, scaled if needed (one
//    D3D11 draw), and NVENC encodes that texture directly – no copy to system memory,
//  - NVENC: P1 preset, "ultra low latency" tuning, no B-frames, no lookahead, CBR with a
//    two-frame buffer, keyframes only when asked for,
//  - the encoded frames go to stdout; the app (direct.js) sends them over UDP at once.
//
//   ss-capture.exe [--rect l,t,r,b | --output n] [--width W --height H] [--fps N] [--bitrate bps]
//   ss-capture.exe --probe            JSON: outputs, adapters, NVENC – then exit
//   ss-capture.exe --probe-capture    JSON: duplicate the output and grab one frame (no NVENC)
//
// stdout: packets – 4-byte tag, uint32 little-endian payload size, payload
//   "INF1"  uint32 width, uint32 height, uint32 fps, uint32 sourceWidth, uint32 sourceHeight, UTF-8 adapter name
//   "VID1"  uint8 keyframe, 3 bytes 0, uint32 encoding time (µs: drawing + NVENC), float64 capture
//           time (ms since 1970, when Windows composed the image), H.264 Annex B
//   "ERR1"  UTF-8 text, then exit (not-nvidia, no-nvenc, nvidia-driver-too-old, display-not-found, …)
// stdin: lines – "K" keyframe, "B <bits/s>" bitrate; closing stdin ends the capture.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcommon.h>
#include <avrt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include "third_party/nvEncodeAPI.h"  // NVIDIA Video Codec SDK 11.1 API header (MIT licence)

static HANDLE g_out = INVALID_HANDLE_VALUE;
static volatile LONG g_quit = 0;
static volatile LONG g_keyframe = 1;  // the first frame is one
static volatile LONG g_bitrateKbps = 0;

template <class F>
static F proc(HMODULE lib, const char* name) {  // GetProcAddress without the function-cast warning
  FARPROC f = lib ? GetProcAddress(lib, name) : nullptr;
  F out;
  memcpy(&out, &f, sizeof out);
  return out;
}

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
static void sendPacket(const char tag[4], const void* a, uint32_t aSize, const void* b = nullptr, uint32_t bSize = 0) {
  const uint32_t size = aSize + bSize;
  unsigned char head[8];
  memcpy(head, tag, 4);
  head[4] = size & 0xFF;
  head[5] = (size >> 8) & 0xFF;
  head[6] = (size >> 16) & 0xFF;
  head[7] = (size >> 24) & 0xFF;
  if (!writeAll(head, 8) || (aSize && !writeAll(a, aSize)) || (bSize && !writeAll(b, bSize))) ExitProcess(0);
}

static int fail(const char* text) {
  sendPacket("ERR1", text, static_cast<uint32_t>(strlen(text)));
  return 1;
}

// "K" → keyframe, "B 80000000" → bitrate; stdin closed (the app stopped us) → quit.
static DWORD WINAPI watchStdin(LPVOID) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  std::string line;
  char buf[256];
  DWORD n = 0;
  while (ReadFile(in, buf, sizeof buf, &n, nullptr) && n > 0) {
    for (DWORD i = 0; i < n; i++) {
      if (buf[i] != '\n') {
        if (line.size() < 64) line += buf[i];
        continue;
      }
      if (line == "K") InterlockedExchange(&g_keyframe, 1);
      else if (line.size() > 2 && line[0] == 'B') InterlockedExchange(&g_bitrateKbps, static_cast<LONG>(_atoi64(line.c_str() + 2) / 1000));
      line.clear();
    }
  }
  InterlockedExchange(&g_quit, 1);
  return 0;
}

/* ------------------------------------------------------------------ clocks */

static double g_qpcToMs = 0;
static double g_epochAtQpc0 = 0;  // ms since 1970 at QPC 0

static double epochNowMs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return (t - 116444736000000000ULL) / 10000.0;
}

static void initClocks() {
  LARGE_INTEGER f, q;
  QueryPerformanceFrequency(&f);
  g_qpcToMs = 1000.0 / static_cast<double>(f.QuadPart);
  QueryPerformanceCounter(&q);
  const double epoch = epochNowMs();
  g_epochAtQpc0 = epoch - q.QuadPart * g_qpcToMs;
}

static double qpcNowMs() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart * g_qpcToMs;
}

/* ------------------------------------------------------------------ options */

struct Options {
  bool hasRect = false;
  RECT rect = {0, 0, 0, 0};
  int output = -1;
  int width = 0;  // the largest picture to send (keeps the aspect ratio); 0 = as the screen
  int height = 0;
  int fps = 60;
  uint32_t bitrate = 50000000;
  bool probe = false;
  bool probeCapture = false;
};

static Options parseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : "";
    if (a == "--probe") o.probe = true;
    else if (a == "--probe-capture") o.probeCapture = true;
    else if (a == "--rect" && i + 1 < argc) {
      long l, t, r, b;
      if (sscanf(v, "%ld,%ld,%ld,%ld", &l, &t, &r, &b) == 4) {
        o.rect = {l, t, r, b};
        o.hasRect = true;
      }
      i++;
    } else if (a == "--output" && i + 1 < argc) o.output = atoi(argv[++i]);
    else if (a == "--width" && i + 1 < argc) o.width = atoi(argv[++i]);
    else if (a == "--height" && i + 1 < argc) o.height = atoi(argv[++i]);
    else if (a == "--fps" && i + 1 < argc) o.fps = std::max(1, std::min(240, atoi(argv[++i])));
    else if (a == "--bitrate" && i + 1 < argc) o.bitrate = static_cast<uint32_t>(std::max<int64_t>(500000, std::min<int64_t>(800000000, _atoi64(argv[++i]))));
  }
  return o;
}

/* ------------------------------------------------------------------ finding the screen */

struct Screen {
  IDXGIAdapter1* adapter = nullptr;
  IDXGIOutput1* output = nullptr;
  DXGI_ADAPTER_DESC1 adapterDesc = {};
  DXGI_OUTPUT_DESC outputDesc = {};
};

static std::string narrow(const wchar_t* w) {
  char buf[256];
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
  return n > 0 ? std::string(buf) : std::string();
}

static std::string jsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out;
}

static long overlap(const RECT& a, const RECT& b) {
  const long w = std::min(a.right, b.right) - std::max(a.left, b.left);
  const long h = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
  return w > 0 && h > 0 ? w * h : 0;
}

/** The output the user picked: the same desktop rectangle (or index); null when none. */
static bool findScreen(const Options& o, Screen& s) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
  long best = -1;
  int index = 0;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
    IDXGIOutput* out = nullptr;
    for (UINT i = 0; adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++, index++) {
      DXGI_OUTPUT_DESC d;
      out->GetDesc(&d);
      long score = -1;
      if (o.hasRect) score = overlap(d.DesktopCoordinates, o.rect);
      else if (o.output >= 0) score = index == o.output ? 1 : -1;
      else score = d.AttachedToDesktop && d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0 ? 1 : 0;  // the main screen
      IDXGIOutput1* out1 = nullptr;
      if (d.AttachedToDesktop && score > best && SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&out1)))) {
        best = score;
        release(s.output);
        release(s.adapter);
        s.output = out1;
        s.adapter = adapter;
        s.adapter->AddRef();
        s.adapter->GetDesc1(&s.adapterDesc);
        s.outputDesc = d;
      }
      release(out);
    }
    release(adapter);
  }
  release(factory);
  return s.output != nullptr && best > 0;
}

/* ------------------------------------------------------------------ NVENC */

typedef NVENCSTATUS(NVENCAPI* PCreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS(NVENCAPI* PMaxVersion)(uint32_t*);

static HMODULE g_nvenc = nullptr;
static NV_ENCODE_API_FUNCTION_LIST g_api = {};

/** "" when NVENC can be used, otherwise why not. */
static std::string loadNvenc() {
  if (g_nvenc) return "";
  g_nvenc = LoadLibraryA("nvEncodeAPI64.dll");
  if (!g_nvenc) return "no-nvenc";
  auto maxVersion = proc<PMaxVersion>(g_nvenc, "NvEncodeAPIGetMaxSupportedVersion");
  auto create = proc<PCreateInstance>(g_nvenc, "NvEncodeAPICreateInstance");
  if (!maxVersion || !create) return "no-nvenc";
  uint32_t version = 0;
  if (maxVersion(&version) != NV_ENC_SUCCESS) return "no-nvenc";
  if (version < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) return "nvidia-driver-too-old";
  g_api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  if (create(&g_api) != NV_ENC_SUCCESS) return "no-nvenc";
  return "";
}

struct Encoder {
  void* session = nullptr;
  NV_ENC_INITIALIZE_PARAMS init = {};
  NV_ENC_CONFIG config = {};
  NV_ENC_OUTPUT_PTR bitstream = nullptr;
  NV_ENC_REGISTERED_PTR input = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps = 60;
  uint32_t bitrate = 0;

  void setRate(uint32_t bps) {
    bitrate = bps;
    NV_ENC_RC_PARAMS& rc = config.rcParams;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.averageBitRate = bps;
    rc.maxBitRate = bps;
    rc.vbvBufferSize = static_cast<uint32_t>(2.0 * bps / fps);  // two frames: a keyframe does not wait long
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.zeroReorderDelay = 1;
    rc.enableLookahead = 0;
  }

  std::string open(ID3D11Device* device, ID3D11Texture2D* texture, uint32_t w, uint32_t h, uint32_t frameRate, uint32_t bps) {
    width = w;
    height = h;
    fps = frameRate;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp = {};
    sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sp.device = device;
    sp.apiVersion = NVENCAPI_VERSION;
    if (g_api.nvEncOpenEncodeSessionEx(&sp, &session) != NV_ENC_SUCCESS) return "nvenc-session";

    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    if (g_api.nvEncGetEncodePresetConfigEx(session, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset) != NV_ENC_SUCCESS) return "nvenc-preset";
    config = preset.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;  // keyframes only when a viewer needs one
    config.frameIntervalP = 1;                     // no B-frames
    setRate(bps);
    NV_ENC_CONFIG_H264& h264 = config.encodeCodecConfig.h264Config;
    h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    h264.repeatSPSPPS = 1;
    h264.chromaFormatIDC = 1;
    // the screen is RGB, converted with BT.709 (what the decoders assume for HD video)
    NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui = h264.h264VUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = 5;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = 1;
    vui.transferCharacteristics = 1;
    vui.colourMatrix = 1;

    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = NV_ENC_PRESET_P1_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = w;
    init.encodeHeight = h;
    init.darWidth = w;
    init.darHeight = h;
    init.maxEncodeWidth = w;
    init.maxEncodeHeight = h;
    init.frameRateNum = frameRate;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.enableEncodeAsync = 0;
    init.encodeConfig = &config;
    if (g_api.nvEncInitializeEncoder(session, &init) != NV_ENC_SUCCESS) return "nvenc-init";

    NV_ENC_CREATE_BITSTREAM_BUFFER bb = {};
    bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (g_api.nvEncCreateBitstreamBuffer(session, &bb) != NV_ENC_SUCCESS) return "nvenc-buffer";
    bitstream = bb.bitstreamBuffer;

    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = w;
    reg.height = h;
    reg.pitch = 0;
    reg.resourceToRegister = texture;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;  // DXGI B8G8R8A8: the encoder converts to YUV itself
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (g_api.nvEncRegisterResource(session, &reg) != NV_ENC_SUCCESS) return "nvenc-register";
    input = reg.registeredResource;
    return "";
  }

  bool reconfigureRate(uint32_t bps) {
    setRate(bps);
    NV_ENC_RECONFIGURE_PARAMS rp = {};
    rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    rp.reInitEncodeParams = init;
    rp.reInitEncodeParams.encodeConfig = &config;
    rp.resetEncoder = 0;
    rp.forceIDR = 0;
    return g_api.nvEncReconfigureEncoder(session, &rp) == NV_ENC_SUCCESS;
  }

  /** Encodes the registered texture; the H.264 bytes are appended to out. */
  bool encode(bool keyframe, uint64_t timestamp, std::vector<uint8_t>& out, bool& isKey) {
    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = input;
    if (g_api.nvEncMapInputResource(session, &map) != NV_ENC_SUCCESS) return false;
    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = width;
    pic.inputHeight = height;
    pic.outputBitstream = bitstream;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = timestamp;
    if (keyframe) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    bool ok = g_api.nvEncEncodePicture(session, &pic) == NV_ENC_SUCCESS;
    if (ok) {
      NV_ENC_LOCK_BITSTREAM lock = {};
      lock.version = NV_ENC_LOCK_BITSTREAM_VER;
      lock.outputBitstream = bitstream;
      lock.doNotWait = 0;
      ok = g_api.nvEncLockBitstream(session, &lock) == NV_ENC_SUCCESS;
      if (ok) {
        const uint8_t* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
        out.insert(out.end(), p, p + lock.bitstreamSizeInBytes);
        isKey = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
        g_api.nvEncUnlockBitstream(session, bitstream);
      }
    }
    g_api.nvEncUnmapInputResource(session, map.mappedResource);
    return ok;
  }

  void close() {
    if (!session) return;
    if (input) g_api.nvEncUnregisterResource(session, input);
    if (bitstream) g_api.nvEncDestroyBitstreamBuffer(session, bitstream);
    g_api.nvEncDestroyEncoder(session);
    session = nullptr;
    input = nullptr;
    bitstream = nullptr;
  }
};

/* ------------------------------------------------------------------ GPU drawing (copy, pointer, scaling) */

static const char kShader[] =
    "cbuffer Quad : register(b0) { float4 rect; };\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  float2 c = float2(id & 1, id >> 1);\n"
    "  V o;\n"
    "  o.pos = float4(lerp(rect.x, rect.z, c.x) * 2 - 1, 1 - lerp(rect.y, rect.w, c.y) * 2, 0, 1);\n"
    "  o.uv = c;\n"
    "  return o;\n"
    "}\n"
    "float4 ps(V i) : SV_TARGET { return tex.Sample(smp, i.uv); }\n";

typedef HRESULT(WINAPI* PD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

struct Painter {
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11Buffer* quad = nullptr;
  ID3D11SamplerState* sampler = nullptr;
  ID3D11BlendState* alpha = nullptr;
  ID3D11RasterizerState* raster = nullptr;

  bool init(ID3D11Device* dev) {
    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = proc<PD3DCompile>(lib, "D3DCompile");
    if (!compile) return false;
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    bool ok = SUCCEEDED(compile(kShader, sizeof kShader - 1, "ss-capture", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, nullptr)) &&
              SUCCEEDED(compile(kShader, sizeof kShader - 1, "ss-capture", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, nullptr)) &&
              SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) &&
              SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps));
    release(vsb);
    release(psb);
    if (!ok) return false;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &quad)) && SUCCEEDED(dev->CreateSamplerState(&sd, &sampler)) &&
           SUCCEEDED(dev->CreateBlendState(&bl, &alpha)) && SUCCEEDED(dev->CreateRasterizerState(&rd, &raster));
  }

  /** Draws src into the bound target over rect (0..1, top-left origin). */
  void draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, const float rect[4], bool blend) {
    ctx->UpdateSubresource(quad, 0, nullptr, rect, 0, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &quad);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->RSSetState(raster);
    const float factor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blend ? alpha : nullptr, factor, 0xFFFFFFFF);
    ctx->Draw(4, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
  }

  void close() {
    release(vs);
    release(ps);
    release(quad);
    release(sampler);
    release(alpha);
    release(raster);
  }
};

/* ------------------------------------------------------------------ the mouse pointer */

struct Pointer {
  bool visible = false;
  long x = 0, y = 0;
  UINT width = 0, height = 0;
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  std::vector<uint8_t> shape;

  // Windows gives colour, masked-colour or monochrome (AND/XOR) shapes: all become BGRA with alpha.
  void update(ID3D11Device* dev, IDXGIOutputDuplication* dupl, const DXGI_OUTDUPL_FRAME_INFO& fi) {
    if (fi.LastMouseUpdateTime.QuadPart) {
      visible = fi.PointerPosition.Visible != 0;
      x = fi.PointerPosition.Position.x;
      y = fi.PointerPosition.Position.y;
    }
    if (!fi.PointerShapeBufferSize) return;
    shape.resize(fi.PointerShapeBufferSize);
    UINT needed = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
    if (FAILED(dupl->GetFramePointerShape(static_cast<UINT>(shape.size()), shape.data(), &needed, &si))) return;
    const bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const UINT w = si.Width;
    const UINT h = mono ? si.Height / 2 : si.Height;
    if (!w || !h) return;
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    for (UINT row = 0; row < h; row++) {
      for (UINT col = 0; col < w; col++) {
        uint32_t c;
        if (mono) {
          const uint8_t bit = 0x80 >> (col % 8);
          const bool andBit = (shape[row * si.Pitch + col / 8] & bit) != 0;
          const bool xorBit = (shape[(row + h) * si.Pitch + col / 8] & bit) != 0;
          c = andBit ? (xorBit ? 0xFF000000 : 0x00000000) : (xorBit ? 0xFFFFFFFF : 0xFF000000);  // inverting pixels drawn black
        } else {
          memcpy(&c, &shape[row * si.Pitch + col * 4], 4);
          if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
            // alpha byte = mask: 0 → this colour, 0xFF → XOR with the screen (drawn when not black)
            c = (c >> 24) ? ((c & 0x00FFFFFF) ? (c | 0xFF000000) : 0) : (c | 0xFF000000);
          }
        }
        px[static_cast<size_t>(row) * w + col] = c;
      }
    }
    release(srv);
    release(tex);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {px.data(), w * 4, 0};
    if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &tex))) dev->CreateShaderResourceView(tex, nullptr, &srv);
    width = w;
    height = h;
  }

  void close() {
    release(srv);
    release(tex);
  }
};

/* ------------------------------------------------------------------ one capture session */

static ID3D11Texture2D* makeTexture(ID3D11Device* dev, UINT w, UINT h, UINT bind, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = bind;
  ID3D11Texture2D* t = nullptr;
  return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &t)) ? t : nullptr;
}

static void targetSize(const Options& o, UINT srcW, UINT srcH, UINT& w, UINT& h) {
  double scale = 1;
  if (o.width > 0 && o.height > 0) {
    const bool landscape = srcW >= srcH;
    const double maxW = landscape ? o.width : o.height;
    const double maxH = landscape ? o.height : o.width;
    scale = std::max({1.0, srcW / maxW, srcH / maxH});
  }
  scale = std::max({scale, srcW / 4096.0, srcH / 4096.0});  // H.264 hardware limit
  w = std::max<UINT>(16, static_cast<UINT>(srcW / scale) & ~1u);
  h = std::max<UINT>(16, static_cast<UINT>(srcH / scale) & ~1u);
}

// Duplicates the screen and encodes until quit; returns "" to start over (screen mode changed) or an error.
static std::string runSession(const Options& opts, bool probeCapture) {
  Screen screen;
  if (!findScreen(opts, screen)) return "display-not-found";
  struct Cleanup {
    Screen& s;
    ~Cleanup() {
      release(s.output);
      release(s.adapter);
    }
  } cleanScreen{screen};
  const bool nvidia = screen.adapterDesc.VendorId == 0x10DE;
  if (!nvidia && !probeCapture) return "display-not-on-nvidia";

  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  if (FAILED(D3D11CreateDevice(screen.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return "d3d11";
  // the GPU work of this process goes first (a game next to it cannot starve it)
  IDXGIDevice* dxgiDev = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev)))) {
    dxgiDev->SetGPUThreadPriority(7);
    release(dxgiDev);
  }

  IDXGIOutputDuplication* dupl = nullptr;
  HRESULT hr = screen.output->DuplicateOutput(dev, &dupl);
  if (FAILED(hr)) {
    release(ctx);
    release(dev);
    return hr == E_ACCESSDENIED ? "access-denied" : hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ? "busy" : "duplication-failed";
  }
  DXGI_OUTDUPL_DESC dd;
  dupl->GetDesc(&dd);
  const UINT srcW = dd.ModeDesc.Width;
  const UINT srcH = dd.ModeDesc.Height;

  if (probeCapture) {
    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource* res = nullptr;
    hr = dupl->AcquireNextFrame(2000, &fi, &res);
    printf("{\"ok\":%s,\"width\":%u,\"height\":%u,\"rotation\":%d,\"adapter\":\"%s\",\"nvidia\":%s,\"frame\":\"0x%08lX\"}\n",
           SUCCEEDED(hr) || hr == DXGI_ERROR_WAIT_TIMEOUT ? "true" : "false", srcW, srcH, static_cast<int>(dd.Rotation),
           jsonEscape(narrow(screen.adapterDesc.Description)).c_str(), nvidia ? "true" : "false", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
      release(res);
      dupl->ReleaseFrame();
    }
    release(dupl);
    release(ctx);
    release(dev);
    return "probe";
  }
  if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
    release(dupl);
    release(ctx);
    release(dev);
    return "rotated-display";
  }

  UINT outW, outH;
  targetSize(opts, srcW, srcH, outW, outH);
  const DXGI_FORMAT srcFormat = dd.ModeDesc.Format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_B8G8R8A8_UNORM : dd.ModeDesc.Format;
  ID3D11Texture2D* copy = makeTexture(dev, srcW, srcH, D3D11_BIND_SHADER_RESOURCE, srcFormat);
  ID3D11Texture2D* frame = makeTexture(dev, outW, outH, D3D11_BIND_RENDER_TARGET);
  ID3D11ShaderResourceView* copySrv = nullptr;
  ID3D11RenderTargetView* frameRtv = nullptr;
  Painter painter;
  Pointer pointer;
  Encoder enc;
  std::string error;
  if (!copy || !frame || FAILED(dev->CreateShaderResourceView(copy, nullptr, &copySrv)) || FAILED(dev->CreateRenderTargetView(frame, nullptr, &frameRtv))) error = "textures";
  else if (!painter.init(dev)) error = "shaders";
  else error = enc.open(dev, frame, outW, outH, static_cast<uint32_t>(opts.fps), g_bitrateKbps ? static_cast<uint32_t>(g_bitrateKbps) * 1000u : opts.bitrate);

  if (error.empty()) {
    std::string info(20, '\0');
    const uint32_t vals[5] = {outW, outH, static_cast<uint32_t>(opts.fps), srcW, srcH};
    memcpy(&info[0], vals, 20);
    info += narrow(screen.adapterDesc.Description);
    sendPacket("INF1", info.data(), static_cast<uint32_t>(info.size()));
    InterlockedExchange(&g_keyframe, 1);

    const double interval = 1000.0 / opts.fps;
    double lastEncode = -1e9;
    double captureMs = epochNowMs();
    bool pending = false;  // a new image (or pointer move) not encoded yet
    bool haveImage = false;
    std::vector<uint8_t> out;
    uint32_t lastKbps = enc.bitrate / 1000;

    auto encodeNow = [&]() -> bool {
      const double started = qpcNowMs();
      // the desktop (scaled to the sent size), then the pointer on it
      ctx->OMSetRenderTargets(1, &frameRtv, nullptr);
      D3D11_VIEWPORT vp = {0, 0, static_cast<float>(outW), static_cast<float>(outH), 0, 1};
      ctx->RSSetViewports(1, &vp);
      const float full[4] = {0, 0, 1, 1};
      painter.draw(ctx, copySrv, full, false);
      if (pointer.visible && pointer.srv) {
        const float r[4] = {static_cast<float>(pointer.x) / srcW, static_cast<float>(pointer.y) / srcH,
                            static_cast<float>(pointer.x + static_cast<long>(pointer.width)) / srcW, static_cast<float>(pointer.y + static_cast<long>(pointer.height)) / srcH};
        painter.draw(ctx, pointer.srv, r, true);
      }
      out.assign(16, 0);
      bool isKey = false;
      const bool key = InterlockedExchange(&g_keyframe, 0) != 0;
      if (!enc.encode(key, static_cast<uint64_t>(captureMs * 1000), out, isKey)) return false;
      const uint32_t encodeUs = static_cast<uint32_t>((qpcNowMs() - started) * 1000);
      out[0] = isKey ? 1 : 0;
      memcpy(&out[4], &encodeUs, 4);
      memcpy(&out[8], &captureMs, 8);
      sendPacket("VID1", out.data(), static_cast<uint32_t>(out.size()));
      lastEncode = qpcNowMs();
      pending = false;
      return true;
    };

    while (!g_quit) {
      const uint32_t kbps = static_cast<uint32_t>(g_bitrateKbps);
      if (kbps && kbps != lastKbps) {
        enc.reconfigureRate(kbps * 1000u);
        lastKbps = kbps;
      }
      const double sinceEncode = qpcNowMs() - lastEncode;
      // waiting for the next image; with one pending, only until it may be sent
      const UINT timeout = pending ? static_cast<UINT>(std::max(0.0, interval - sinceEncode)) : 50;
      DXGI_OUTDUPL_FRAME_INFO fi;
      IDXGIResource* res = nullptr;
      hr = dupl->AcquireNextFrame(timeout, &fi, &res);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // a keyframe asked for while the screen stands still: the last image again
        if (haveImage && (pending || g_keyframe) && qpcNowMs() - lastEncode >= interval * 0.95 && !encodeNow()) {
          error = "nvenc-encode";
          break;
        }
        continue;
      }
      if (hr == DXGI_ERROR_ACCESS_LOST) break;  // mode change, secure desktop, full-screen switch: start over
      if (FAILED(hr)) {
        error = "capture-failed";
        break;
      }
      if (fi.LastPresentTime.QuadPart) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
          ctx->CopyResource(copy, tex);
          release(tex);
          captureMs = g_epochAtQpc0 + fi.LastPresentTime.QuadPart * g_qpcToMs;  // when Windows composed it
          haveImage = true;
          pending = true;
        }
      }
      if (fi.LastMouseUpdateTime.QuadPart) {
        pointer.update(dev, dupl, fi);
        if (haveImage) pending = true;
        if (!fi.LastPresentTime.QuadPart) captureMs = epochNowMs();
      }
      release(res);
      dupl->ReleaseFrame();
      if (pending && qpcNowMs() - lastEncode >= interval * 0.95 && !encodeNow()) {
        error = "nvenc-encode";
        break;
      }
    }
  }

  enc.close();
  pointer.close();
  painter.close();
  release(frameRtv);
  release(copySrv);
  release(frame);
  release(copy);
  release(dupl);
  release(ctx);
  release(dev);
  return error;
}

/* ------------------------------------------------------------------ probe */

static int probe() {
  const std::string nvenc = loadNvenc();
  uint32_t maxVersion = 0;
  if (g_nvenc) {
    auto mv = proc<PMaxVersion>(g_nvenc, "NvEncodeAPIGetMaxSupportedVersion");
    if (mv) mv(&maxVersion);
  }
  printf("{\"nvenc\":%s,\"nvencError\":\"%s\",\"nvencMaxApi\":\"%u.%u\",\"outputs\":[", nvenc.empty() ? "true" : "false", nvenc.c_str(), maxVersion >> 4, maxVersion & 0xF);
  IDXGIFactory1* factory = nullptr;
  int index = 0;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
      DXGI_ADAPTER_DESC1 ad;
      adapter->GetDesc1(&ad);
      IDXGIOutput* out = nullptr;
      for (UINT i = 0; adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++, index++) {
        DXGI_OUTPUT_DESC d;
        out->GetDesc(&d);
        const RECT& r = d.DesktopCoordinates;
        printf("%s{\"index\":%d,\"adapter\":\"%s\",\"nvidia\":%s,\"rect\":[%ld,%ld,%ld,%ld],\"attached\":%s,\"rotation\":%d}", index ? "," : "", index,
               jsonEscape(narrow(ad.Description)).c_str(), ad.VendorId == 0x10DE ? "true" : "false", r.left, r.top, r.right, r.bottom,
               d.AttachedToDesktop ? "true" : "false", static_cast<int>(d.Rotation));
        release(out);
      }
      release(adapter);
    }
    release(factory);
  }
  printf("]}\n");
  return 0;
}

/* ------------------------------------------------------------------ main */

typedef BOOL(WINAPI* PSetDpiContext)(HANDLE);

int main(int argc, char** argv) {
  // per-monitor DPI aware: the duplication sees the real pixels (and is allowed at all)
  HMODULE user32 = GetModuleHandleA("user32.dll");
  auto setDpi = proc<PSetDpiContext>(user32, "SetProcessDpiAwarenessContext");
  if (setDpi) setDpi(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

  const Options opts = parseArgs(argc, argv);
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  initClocks();
  if (opts.probe) return probe();
  if (opts.probeCapture) {
    const std::string r = runSession(opts, true);
    if (r != "probe") printf("{\"ok\":false,\"error\":\"%s\"}\n", r.c_str());
    return 0;
  }

  const std::string nvenc = loadNvenc();
  if (!nvenc.empty()) return fail(nvenc.c_str());
  HANDLE watcher = CreateThread(nullptr, 0, watchStdin, nullptr, 0, nullptr);
  if (watcher) CloseHandle(watcher);
  if (opts.bitrate) InterlockedExchange(&g_bitrateKbps, static_cast<LONG>(opts.bitrate / 1000));
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  // a lost duplication (screen mode change, UAC prompt, a game switching to full screen) starts over
  int failures = 0;
  while (!g_quit) {
    const std::string error = runSession(opts, false);
    if (g_quit) break;
    if (!error.empty()) {
      if (error == "access-denied" || error == "busy" || error == "capture-failed") {
        if (++failures > 50) return fail(error.c_str());  // ~10 s of a secure desktop is fine, longer is not
        Sleep(200);
        continue;
      }
      return fail(error.c_str());
    }
    failures = 0;
    Sleep(50);
  }
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  return 0;
}
