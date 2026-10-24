#include "player.h"

#include <codecapi.h>
#include <d3d11.h>
#include <d3d10.h> // ID3D10Multithread
#include <dxgi1_5.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "clock.h"
#include "gpu.h"

namespace ss {

namespace {

// IID_ICodecAPI (not linked in every toolchain)
const IID kCodecApi = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

// NV12 → RGB (BT.709, video range), drawn over a letterboxed rectangle
const char* kShader =
    "cbuffer Quad : register(b0) { float4 rect; };\n"
    "Texture2D<float> lum : register(t0);\n"
    "Texture2D<float2> chroma : register(t1);\n"
    "SamplerState smp : register(s0);\n"
    "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  float2 c = float2(id & 1, id >> 1);\n"
    "  V o;\n"
    "  o.pos = float4(lerp(rect.x, rect.z, c.x) * 2 - 1, 1 - lerp(rect.y, rect.w, c.y) * 2, 0, 1);\n"
    "  o.uv = c;\n"
    "  return o;\n"
    "}\n"
    "float4 ps(V i) : SV_TARGET {\n"
    "  float y = (lum.Sample(smp, i.uv) - 0.0627451) * 1.164383;\n"
    "  float2 uv = chroma.Sample(smp, i.uv) - 0.5;\n"
    "  return float4(saturate(float3(y + 1.792741 * uv.y, y - 0.213249 * uv.x - 0.532909 * uv.y, y + 2.112402 * uv.x)), 1);\n"
    "}\n";

struct Frame {
  std::vector<uint8_t> data;
  bool key;
  double capture;
};

}  // namespace

struct VideoPlayer::Impl {
  HWND window;
  VideoPlayer* owner;
  std::thread thread;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Frame> queue;
  bool quit = false, resize = false, resetPending = false;

  // Direct3D
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGISwapChain1> swap;
  HANDLE waitable = nullptr;
  bool tearing = false;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11Buffer> quad;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11RasterizerState> raster;
  ComPtr<ID3D11Texture2D> lumTex, chromaTex; // what is shown (copied out of the decoder)
  ComPtr<ID3D11ShaderResourceView> lumSrv, chromaSrv;
  UINT texW = 0, texH = 0;
  UINT winW = 0, winH = 0;
  ComPtr<ID3D11Texture2D> nv12; // a hardware frame's copy (shader-readable)

  // Media Foundation
  ComPtr<IMFTransform> decoder;
  ComPtr<IMFDXGIDeviceManager> manager;
  bool hardware = false;
  bool providesSamples = false;
  DWORD outSize = 0;
  UINT32 frameW = 0, frameH = 0, stride = 0;
  bool needKey = true;
  LONGLONG sampleTime = 0;
  std::deque<double> captures; // capture times of the frames inside the decoder, in order

  // statistics
  mutable std::mutex statsMu;
  Stats stats;
  std::vector<double> lat;
  int shown = 0;
  double statsAt = 0;

  /* ---- setup */

  bool initDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
      return false;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(dev.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIFactory5> f5;
    BOOL allow = FALSE;
    if (SUCCEEDED(factory.As(&f5)) && SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow))) tearing = allow != FALSE;
    RECT r;
    GetClientRect(window, &r);
    winW = UINT(std::max<LONG>(1, r.right));
    winH = UINT(std::max<LONG>(1, r.bottom));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = winW;
    sd.Height = winH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(factory->CreateSwapChainForHwnd(dev.Get(), window, &sd, nullptr, nullptr, &swap))) return false;
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swap.As(&sc2))) {
      sc2->SetMaximumFrameLatency(1);
      waitable = sc2->GetFrameLatencyWaitableObject();
    }
    ComPtr<ID3DBlob> vsb, psb;
    if (!compileShader(kShader, "vs", "vs_4_0", vsb) || !compileShader(kShader, "ps", "ps_4_0", psb)) return false;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &quad);
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&smp, &sampler);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &raster);
    makeTarget();
    return true;
  }

  void makeTarget() {
    rtv.Reset();
    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);
  }

  void doResize() {
    RECT r;
    GetClientRect(window, &r);
    UINT w = UINT(std::max<LONG>(1, r.right)), h = UINT(std::max<LONG>(1, r.bottom));
    if (w == winW && h == winH) return;
    winW = w;
    winH = h;
    rtv.Reset();
    ctx->ClearState();
    ctx->Flush();
    swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    makeTarget();
    draw(); // the last picture at the new size
  }

  bool initDecoder() {
    decoder.Reset();
    manager.Reset();
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder)))) return false;
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(decoder->GetAttributes(&attrs))) {
      attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE); // a frame out for every frame in, no reordering wait
      UINT32 aware = 0;
      if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware) {
        UINT token = 0;
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &manager)) && SUCCEEDED(manager->ResetDevice(dev.Get(), token)) &&
            SUCCEEDED(decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())))) {
          hardware = true;
        }
      }
    }
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(decoder->QueryInterface(kCodecApi, reinterpret_cast<void**>(codec.GetAddressOf())))) {
      VARIANT v;
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = 1;
      codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(decoder->SetInputType(0, in.Get(), 0))) return false;
    if (!pickOutput()) return false;
    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    needKey = true;
    captures.clear();
    return true;
  }

  bool pickOutput() {
    for (DWORD i = 0;; i++) {
      ComPtr<IMFMediaType> t;
      if (FAILED(decoder->GetOutputAvailableType(0, i, &t))) return false;
      GUID sub;
      if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12 && SUCCEEDED(decoder->SetOutputType(0, t.Get(), 0))) {
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &frameW, &frameH);
        UINT32 s = 0;
        stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? s : frameW;
        MFT_OUTPUT_STREAM_INFO info = {};
        decoder->GetOutputStreamInfo(0, &info);
        providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outSize = info.cbSize;
        return true;
      }
    }
  }

  /* ---- decoding */

  void decode(Frame& f) {
    if (needKey && !f.key) {
      if (owner->needKeyframe) owner->needKeyframe();
      return;
    }
    needKey = false;
    ComPtr<IMFMediaBuffer> buf;
    MFCreateMemoryBuffer(DWORD(f.data.size()), &buf);
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    std::memcpy(p, f.data.data(), f.data.size());
    buf->Unlock();
    buf->SetCurrentLength(DWORD(f.data.size()));
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(sampleTime);
    sample->SetSampleDuration(166666);
    sampleTime += 166666;
    if (f.key) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    HRESULT hr = decoder->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      pull();
      hr = decoder->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
      failed();
      return;
    }
    captures.push_back(f.capture);
    if (captures.size() > 16) captures.pop_front();
    pull();
  }

  void failed() {
    // start over: a fresh decoder and a keyframe
    initDecoder();
    if (owner->needKeyframe) owner->needKeyframe();
  }

  /** Takes every frame the decoder has ready; only the newest is drawn. */
  void pull() {
    bool got = false;
    double capture = 0;
    for (int guard = 0; guard < 8; guard++) {
      MFT_OUTPUT_DATA_BUFFER ob = {};
      ComPtr<IMFSample> mine;
      if (!providesSamples) {
        MFCreateSample(&mine);
        ComPtr<IMFMediaBuffer> b;
        MFCreateMemoryBuffer(std::max<DWORD>(outSize, frameW * frameH * 3 / 2), &b);
        mine->AddBuffer(b.Get());
        ob.pSample = mine.Get();
      }
      DWORD status = 0;
      HRESULT hr = decoder->ProcessOutput(0, 1, &ob, &status);
      if (ob.pEvents) ob.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (!pickOutput()) return failed();
        continue;
      }
      if (FAILED(hr) || !ob.pSample) break;
      ComPtr<IMFSample> sample;
      sample.Attach(providesSamples ? ob.pSample : (mine.Get()->AddRef(), mine.Get()));
      capture = captures.empty() ? 0 : captures.front();
      if (!captures.empty()) captures.pop_front();
      if (upload(sample.Get())) got = true;
    }
    if (got) {
      draw();
      measure(capture);
    }
  }

  /** The decoded NV12 picture into the textures the shader reads. */
  bool upload(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;
    ComPtr<IMFDXGIBuffer> dxgi;
    if (SUCCEEDED(buf.As(&dxgi))) {
      ComPtr<ID3D11Texture2D> tex;
      UINT index = 0;
      if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dxgi->GetSubresourceIndex(&index))) return false;
      D3D11_TEXTURE2D_DESC td;
      tex->GetDesc(&td);
      ensureTextures(td.Width, td.Height, true);
      ctx->CopySubresourceRegion(nv12.Get(), 0, 0, 0, 0, tex.Get(), index, nullptr);
      return true;
    }
    // software decoding: the planes come from memory
    ensureTextures(frameW, frameH, false);
    BYTE* p = nullptr;
    LONG pitch = LONG(stride);
    ComPtr<IMF2DBuffer> b2;
    bool locked2d = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch));
    DWORD len = 0;
    if (!locked2d && FAILED(buf->Lock(&p, nullptr, &len))) return false;
    ctx->UpdateSubresource(lumTex.Get(), 0, nullptr, p, UINT(pitch), 0);
    ctx->UpdateSubresource(chromaTex.Get(), 0, nullptr, p + size_t(pitch) * frameH, UINT(pitch), 0);
    if (locked2d) b2->Unlock2D();
    else buf->Unlock();
    return true;
  }

  void ensureTextures(UINT w, UINT h, bool fromGpu) {
    if (w == texW && h == texH && bool(nv12) == fromGpu && lumSrv) return;
    texW = w;
    texH = h;
    lumTex.Reset();
    chromaTex.Reset();
    lumSrv.Reset();
    chromaSrv.Reset();
    nv12.Reset();
    if (fromGpu) {
      // one NV12 texture, read as two planes
      nv12 = makeTexture(dev.Get(), w, h, DXGI_FORMAT_NV12, D3D11_BIND_SHADER_RESOURCE);
      D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
      v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      v.Texture2D.MipLevels = 1;
      v.Format = DXGI_FORMAT_R8_UNORM;
      dev->CreateShaderResourceView(nv12.Get(), &v, &lumSrv);
      v.Format = DXGI_FORMAT_R8G8_UNORM;
      dev->CreateShaderResourceView(nv12.Get(), &v, &chromaSrv);
    } else {
      lumTex = makeTexture(dev.Get(), w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
      chromaTex = makeTexture(dev.Get(), w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_SHADER_RESOURCE);
      dev->CreateShaderResourceView(lumTex.Get(), nullptr, &lumSrv);
      dev->CreateShaderResourceView(chromaTex.Get(), nullptr, &chromaSrv);
    }
  }

  /* ---- drawing */

  void draw() {
    if (!rtv) return;
    // the frame-latency object says when a present will not wait (≤ 1 frame queued)
    if (waitable) WaitForSingleObjectEx(waitable, 16, TRUE);
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(rtv.Get(), black);
    if (lumSrv && chromaSrv && frameW && frameH) {
      // letterbox: the whole picture, its aspect kept
      double sx = double(winW) / frameW, sy = double(winH) / frameH, s = std::min(sx, sy);
      float w = float(frameW * s / winW), h = float(frameH * s / winH);
      float rect[4] = {(1 - w) / 2, (1 - h) / 2, (1 + w) / 2, (1 + h) / 2};
      ctx->UpdateSubresource(quad.Get(), 0, nullptr, rect, 0, 0);
      ID3D11RenderTargetView* target = rtv.Get();
      ctx->OMSetRenderTargets(1, &target, nullptr);
      D3D11_VIEWPORT vp = {0, 0, float(winW), float(winH), 0, 1};
      ctx->RSSetViewports(1, &vp);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      ctx->IASetInputLayout(nullptr);
      ctx->VSSetShader(vs.Get(), nullptr, 0);
      ctx->VSSetConstantBuffers(0, 1, quad.GetAddressOf());
      ctx->PSSetShader(ps.Get(), nullptr, 0);
      ID3D11ShaderResourceView* srvs[2] = {lumSrv.Get(), chromaSrv.Get()};
      ctx->PSSetShaderResources(0, 2, srvs);
      ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
      ctx->RSSetState(raster.Get());
      ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
      ctx->Draw(4, 0);
      ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
      ctx->PSSetShaderResources(0, 2, none);
    }
    swap->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
  }

  void measure(double capture) {
    double now = nowMs();
    double off = owner->clockOffset ? owner->clockOffset() : NAN;
    if (capture > 0 && !std::isnan(off)) lat.push_back(now + off - capture);
    shown++;
    if (now - statsAt >= 1000) {
      std::lock_guard<std::mutex> l(statsMu);
      stats.width = int(frameW);
      stats.height = int(frameH);
      stats.fps = statsAt ? shown * 1000.0 / (now - statsAt) : 0;
      stats.hardware = hardware;
      if (!lat.empty()) {
        std::nth_element(lat.begin(), lat.begin() + long(lat.size() / 2), lat.end());
        stats.latencyMs = lat[lat.size() / 2];
      }
      lat.clear();
      shown = 0;
      statsAt = now;
    }
  }

  /* ---- the thread */

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    bool ok = initDevice() && initDecoder();
    while (ok) {
      Frame f;
      {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return quit || resize || resetPending || !queue.empty(); });
        if (quit) break;
        if (resize) {
          resize = false;
          l.unlock();
          doResize();
          continue;
        }
        if (resetPending) {
          resetPending = false;
          queue.clear();
          l.unlock();
          initDecoder();
          continue;
        }
        f = std::move(queue.front());
        queue.pop_front();
      }
      decode(f);
    }
    decoder.Reset();
    manager.Reset();
    swap.Reset();
    CoUninitialize();
  }
};

VideoPlayer::VideoPlayer(HWND window) : impl_(new Impl) {
  impl_->window = window;
  impl_->owner = this;
  impl_->thread = std::thread([this] { impl_->run(); });
}

VideoPlayer::~VideoPlayer() {
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->quit = true;
  }
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
}

void VideoPlayer::push(const uint8_t* data, size_t n, bool key, double captureMs) {
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    // the decoder fell behind badly (a slow computer): drop to the next keyframe
    if (impl_->queue.size() > 30 && key) impl_->queue.clear();
    impl_->queue.push_back(Frame{std::vector<uint8_t>(data, data + n), key, captureMs});
  }
  impl_->cv.notify_all();
}

void VideoPlayer::resized() {
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->resize = true;
  }
  impl_->cv.notify_all();
}

void VideoPlayer::reset() {
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->resetPending = true;
  }
  impl_->cv.notify_all();
}

VideoPlayer::Stats VideoPlayer::stats() const {
  std::lock_guard<std::mutex> l(impl_->statsMu);
  return impl_->stats;
}

}  // namespace ss
