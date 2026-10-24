// Direct3D 11 helpers shared by the capture (scaling, the pointer) and the player (NV12 → screen).
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <wrl/client.h>

#include <cstring>
#include <string>

namespace ss {

using Microsoft::WRL::ComPtr;

/** Compiles HLSL at run time (d3dcompiler_47.dll is part of Windows 10+). */
inline bool compileShader(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
  typedef HRESULT(WINAPI * Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
  static Compile compile = [] {
    HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
    FARPROC f = lib ? GetProcAddress(lib, "D3DCompile") : nullptr;
    Compile c;
    std::memcpy(&c, &f, sizeof c);
    return c;
  }();
  if (!compile) return false;
  return SUCCEEDED(compile(src, std::strlen(src), entry, nullptr, nullptr, entry, target, 0, 0, out.GetAddressOf(), nullptr));
}

inline ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT format, UINT bind) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = bind;
  ComPtr<ID3D11Texture2D> t;
  dev->CreateTexture2D(&td, nullptr, t.GetAddressOf());
  return t;
}

/** Draws a texture into the bound render target over a rectangle (0..1, top-left origin). */
class QuadPainter {
 public:
  bool init(ID3D11Device* dev) {
    static const char* src =
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
    ComPtr<ID3DBlob> vsb, psb;
    if (!compileShader(src, "vs", "vs_4_0", vsb) || !compileShader(src, "ps", "ps_4_0", psb)) return false;
    if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_)))
      return false;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC bl = {};
    auto& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &quad_)) && SUCCEEDED(dev->CreateSamplerState(&sd, &sampler_)) &&
           SUCCEEDED(dev->CreateBlendState(&bl, &alpha_)) && SUCCEEDED(dev->CreateRasterizerState(&rd, &raster_));
  }

  void draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, const float rect[4], bool blend) {
    ctx->UpdateSubresource(quad_.Get(), 0, nullptr, rect, 0, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, quad_.GetAddressOf());
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->RSSetState(raster_.Get());
    const float factor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blend ? alpha_.Get() : nullptr, factor, 0xFFFFFFFF);
    ctx->Draw(4, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
  }

 private:
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11Buffer> quad_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11BlendState> alpha_;
  ComPtr<ID3D11RasterizerState> raster_;
};

inline std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

inline std::wstring widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}

}  // namespace ss
