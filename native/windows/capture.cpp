#include "capture.h"

#include <d3d10.h> // ID3D10Multithread
#include <cwchar>

#include <algorithm>

#include "clock.h"

namespace ss {

std::vector<Monitor> listMonitors() {
  std::vector<Monitor> out;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) return out;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; a++) {
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_OUTPUT_DESC d;
      output->GetDesc(&d);
      if (!d.AttachedToDesktop) continue;
      const RECT& r = d.DesktopCoordinates;
      MONITORINFO mi{sizeof mi};
      GetMonitorInfoW(d.Monitor, &mi);
      Monitor m{L"", r, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0};
      out.push_back(m);
    }
  }
  std::sort(out.begin(), out.end(), [](const Monitor& a, const Monitor& b) { return a.primary != b.primary ? a.primary : a.rect.left < b.rect.left; });
  for (size_t i = 0; i < out.size(); i++) {
    wchar_t name[96];
    swprintf(name, 96, L"%zu. képernyő (%ld×%ld)%ls", i + 1, out[i].rect.right - out[i].rect.left, out[i].rect.bottom - out[i].rect.top, out[i].primary ? L" – fő" : L"");
    out[i].name = name;
  }
  return out;
}

namespace {

long overlap(const RECT& a, const RECT& b) {
  long w = std::min(a.right, b.right) - std::max(a.left, b.left);
  long h = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
  return w > 0 && h > 0 ? w * h : 0;
}

double qpcToNowMs(LONGLONG qpc) {
  static double perMs = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart) / 1000.0;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return nowMs() - double(now.QuadPart - qpc) / perMs; // the engine's clock
}

/** The mouse pointer: Windows gives colour, masked-colour or monochrome shapes – all become BGRA with alpha. */
struct Pointer {
  bool visible = false;
  long x = 0, y = 0;
  UINT width = 0, height = 0;
  ComPtr<ID3D11ShaderResourceView> srv;
  std::vector<uint8_t> shape;

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
    if (FAILED(dupl->GetFramePointerShape(UINT(shape.size()), shape.data(), &needed, &si))) return;
    bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    UINT w = si.Width, h = mono ? si.Height / 2 : si.Height;
    if (!w || !h) return;
    std::vector<uint32_t> px(size_t(w) * h);
    for (UINT row = 0; row < h; row++) {
      for (UINT col = 0; col < w; col++) {
        uint32_t c;
        if (mono) {
          uint8_t bit = uint8_t(0x80 >> (col % 8));
          bool andBit = (shape[row * si.Pitch + col / 8] & bit) != 0;
          bool xorBit = (shape[(row + h) * si.Pitch + col / 8] & bit) != 0;
          c = andBit ? (xorBit ? 0xFF000000 : 0x00000000) : (xorBit ? 0xFFFFFFFF : 0xFF000000); // inverting pixels drawn black
        } else {
          std::memcpy(&c, &shape[row * si.Pitch + col * 4], 4);
          if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) c = (c >> 24) ? ((c & 0x00FFFFFF) ? (c | 0xFF000000) : 0) : (c | 0xFF000000);
        }
        px[size_t(row) * w + col] = c;
      }
    }
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
    ComPtr<ID3D11Texture2D> tex;
    srv.Reset();
    if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &tex))) dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    width = w;
    height = h;
  }
};

}  // namespace

struct ScreenCapture::Impl {
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput1> output;
  DXGI_ADAPTER_DESC1 adapterDesc{};
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGIOutputDuplication> dupl;
  ComPtr<ID3D11Texture2D> copy, frame;
  ComPtr<ID3D11ShaderResourceView> copySrv;
  ComPtr<ID3D11RenderTargetView> frameRtv;
  QuadPainter painter;
  Pointer pointer;
  UINT srcW = 0, srcH = 0, outW = 0, outH = 0;
  bool haveImage = false;
};

ScreenCapture::ScreenCapture() : impl_(new Impl) {}
ScreenCapture::~ScreenCapture() { close(); }

std::string ScreenCapture::open(const RECT& monitor, int maxW, int maxH) {
  close();
  Impl& s = *impl_;
  // the output showing that part of the desktop, on whichever adapter drives it
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) return "d3d11";
  long best = 0;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; a++) {
    ComPtr<IDXGIOutput> out;
    for (UINT i = 0; adapter->EnumOutputs(i, out.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_OUTPUT_DESC d;
      out->GetDesc(&d);
      long score = overlap(d.DesktopCoordinates, monitor);
      ComPtr<IDXGIOutput1> out1;
      if (d.AttachedToDesktop && score > best && SUCCEEDED(out.As(&out1))) {
        best = score;
        s.output = out1;
        s.adapter = adapter;
        adapter->GetDesc1(&s.adapterDesc);
      }
    }
  }
  if (!s.output) return "display-not-found";
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
  if (FAILED(D3D11CreateDevice(s.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 3,
                               D3D11_SDK_VERSION, &s.dev, nullptr, &s.ctx)) &&
      FAILED(D3D11CreateDevice(s.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION, &s.dev, nullptr, &s.ctx)))
    return "d3d11";
  ComPtr<ID3D10Multithread> mt;
  if (SUCCEEDED(s.ctx.As(&mt))) mt->SetMultithreadProtected(TRUE); // the encoder may use the device from its own thread
  // the GPU work of this process goes first (a game next to it cannot starve it)
  ComPtr<IDXGIDevice> dxgiDev;
  if (SUCCEEDED(s.dev.As(&dxgiDev))) dxgiDev->SetGPUThreadPriority(7);
  HRESULT hr = s.output->DuplicateOutput(s.dev.Get(), &s.dupl);
  if (FAILED(hr)) return hr == E_ACCESSDENIED ? "access-denied" : hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ? "busy" : "duplication-failed";
  DXGI_OUTDUPL_DESC dd;
  s.dupl->GetDesc(&dd);
  if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) return "rotated-display";
  s.srcW = dd.ModeDesc.Width;
  s.srcH = dd.ModeDesc.Height;
  double scale = 1;
  if (maxW > 0 && maxH > 0) {
    bool landscape = s.srcW >= s.srcH;
    double mw = landscape ? maxW : maxH, mh = landscape ? maxH : maxW;
    scale = std::max({1.0, s.srcW / mw, s.srcH / mh});
  }
  scale = std::max({scale, s.srcW / 4096.0, s.srcH / 4096.0}); // H.264 hardware limit
  s.outW = std::max<UINT>(16, UINT(s.srcW / scale) & ~1u);
  s.outH = std::max<UINT>(16, UINT(s.srcH / scale) & ~1u);
  DXGI_FORMAT fmt = dd.ModeDesc.Format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_B8G8R8A8_UNORM : dd.ModeDesc.Format;
  s.copy = makeTexture(s.dev.Get(), s.srcW, s.srcH, fmt, D3D11_BIND_SHADER_RESOURCE);
  s.frame = makeTexture(s.dev.Get(), s.outW, s.outH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
  if (!s.copy || !s.frame || FAILED(s.dev->CreateShaderResourceView(s.copy.Get(), nullptr, &s.copySrv)) ||
      FAILED(s.dev->CreateRenderTargetView(s.frame.Get(), nullptr, &s.frameRtv)))
    return "textures";
  if (!s.painter.init(s.dev.Get())) return "shaders";
  return "";
}

void ScreenCapture::close() {
  Impl fresh;
  std::swap(*impl_, fresh);
}

int ScreenCapture::acquire(unsigned timeoutMs, double& captureMs) {
  Impl& s = *impl_;
  if (!s.dupl) return -2;
  DXGI_OUTDUPL_FRAME_INFO fi;
  ComPtr<IDXGIResource> res;
  HRESULT hr = s.dupl->AcquireNextFrame(timeoutMs, &fi, &res);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
  if (hr == DXGI_ERROR_ACCESS_LOST) return -1; // mode change, secure desktop, full-screen switch
  if (FAILED(hr)) return -2;
  int got = 0;
  if (fi.LastPresentTime.QuadPart) {
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res.As(&tex))) {
      s.ctx->CopyResource(s.copy.Get(), tex.Get());
      captureMs = qpcToNowMs(fi.LastPresentTime.QuadPart); // when Windows composed it
      s.haveImage = true;
      got = 1;
    }
  }
  if (fi.LastMouseUpdateTime.QuadPart) {
    s.pointer.update(s.dev.Get(), s.dupl.Get(), fi);
    if (s.haveImage && !got) {
      captureMs = nowMs();
      got = 1;
    }
  }
  res.Reset();
  s.dupl->ReleaseFrame();
  return got;
}

void ScreenCapture::render() {
  Impl& s = *impl_;
  if (!s.haveImage) return;
  ID3D11RenderTargetView* rtv = s.frameRtv.Get();
  s.ctx->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp = {0, 0, float(s.outW), float(s.outH), 0, 1};
  s.ctx->RSSetViewports(1, &vp);
  const float full[4] = {0, 0, 1, 1};
  s.painter.draw(s.ctx.Get(), s.copySrv.Get(), full, false);
  if (s.pointer.visible && s.pointer.srv) {
    const float r[4] = {float(s.pointer.x) / s.srcW, float(s.pointer.y) / s.srcH, float(s.pointer.x + long(s.pointer.width)) / s.srcW,
                        float(s.pointer.y + long(s.pointer.height)) / s.srcH};
    s.painter.draw(s.ctx.Get(), s.pointer.srv.Get(), r, true);
  }
  ID3D11RenderTargetView* none = nullptr;
  s.ctx->OMSetRenderTargets(1, &none, nullptr);
}

ID3D11Device* ScreenCapture::device() const { return impl_->dev.Get(); }
ID3D11DeviceContext* ScreenCapture::context() const { return impl_->ctx.Get(); }
ID3D11Texture2D* ScreenCapture::frame() const { return impl_->frame.Get(); }
int ScreenCapture::width() const { return int(impl_->outW); }
int ScreenCapture::height() const { return int(impl_->outH); }
int ScreenCapture::sourceWidth() const { return int(impl_->srcW); }
int ScreenCapture::sourceHeight() const { return int(impl_->srcH); }
bool ScreenCapture::nvidia() const { return impl_->adapterDesc.VendorId == 0x10DE; }
LUID ScreenCapture::adapterLuid() const { return impl_->adapterDesc.AdapterLuid; }
std::string ScreenCapture::adapterName() const { return narrow(impl_->adapterDesc.Description); }

}  // namespace ss
