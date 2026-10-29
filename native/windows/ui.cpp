#include "ui.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace ss::ui {

Color rgb(uint32_t hex, float a) { return {float((hex >> 16) & 0xFF) / 255.0f, float((hex >> 8) & 0xFF) / 255.0f, float(hex & 0xFF) / 255.0f, a}; }

Color mix(const Color& a, const Color& b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Color withAlpha(const Color& c, float a) { return {c.r, c.g, c.b, c.a * a}; }

namespace {
const float kPi = 3.14159265f;
/** Wine draws gradient brushes without their transparency and radial ones solid: there, plainer stand-ins. */
bool isWine() {
  static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
  return wine;
}
D2D1_COLOR_F d2d(const Color& c, float alpha) { return D2D1::ColorF(c.r, c.g, c.b, std::clamp(c.a * alpha, 0.0f, 1.0f)); }
D2D1_ROUNDED_RECT rr(const Rect& r, float radius) { return D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), radius, radius); }
D2D1_RECT_F rf(const Rect& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }
}  // namespace

Canvas::Canvas() {
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
  // the first of these the system has (Windows 11, Windows 10, then anything sensible)
  family_ = L"Segoe UI";
  mono_ = L"Consolas";
  ComPtr<IDWriteFontCollection> fonts;
  if (dwrite_ && SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts, FALSE))) {
    auto first = [&](std::initializer_list<const wchar_t*> names, std::wstring& out) {
      for (const wchar_t* name : names) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists) {
          out = name;
          return;
        }
      }
    };
    first({L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma", L"Arial", L"Liberation Sans", L"DejaVu Sans"}, family_);
    first({L"Cascadia Mono", L"Consolas", L"Liberation Mono", L"DejaVu Sans Mono", L"Courier New"}, mono_);
  }
  if (factory_) {
    D2D1_STROKE_STYLE_PROPERTIES p = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(p, nullptr, 0, round_.GetAddressOf());
  }
}

Canvas::~Canvas() = default;

bool Canvas::begin(HWND wnd, float dpi) {
  if (!factory_) return false;
  RECT rc;
  GetClientRect(wnd, &rc);
  D2D1_SIZE_U size = D2D1::SizeU(UINT32(std::max<LONG>(1, rc.right)), UINT32(std::max<LONG>(1, rc.bottom)));
  if (!target_ || wnd != wnd_) {
    wnd_ = wnd;
    target_.Reset();
    brush_.Reset();
    icons_.clear();
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(wnd, size, D2D1_PRESENT_OPTIONS_NONE), target_.GetAddressOf()))) return false;
    target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.GetAddressOf());
  } else {
    D2D1_SIZE_U now = target_->GetPixelSize();
    if (now.width != size.width || now.height != size.height) target_->Resize(size);
  }
  dpi_ = dpi;
  target_->SetDpi(dpi, dpi);
  width_ = float(size.width) * 96.0f / dpi;
  height_ = float(size.height) * 96.0f / dpi;
  alpha_.assign(1, 1.0f);
  transforms_.assign(1, D2D1::Matrix3x2F::Identity());
  target_->BeginDraw();
  target_->SetTransform(D2D1::Matrix3x2F::Identity());
  return true;
}

void Canvas::end() {
  if (!target_) return;
  last_ = target_->EndDraw();
  if (last_ == HRESULT(D2DERR_RECREATE_TARGET)) {
    target_.Reset();
    brush_.Reset();
    icons_.clear();
  }
}

void Canvas::resized() {
  if (!target_ || !wnd_) return;
  RECT rc;
  GetClientRect(wnd_, &rc);
  target_->Resize(D2D1::SizeU(UINT32(std::max<LONG>(1, rc.right)), UINT32(std::max<LONG>(1, rc.bottom))));
}

ID2D1SolidColorBrush* Canvas::brush(const Color& c) {
  brush_->SetColor(d2d(c, alpha_.back()));
  return brush_.Get();
}

void Canvas::clear(const Color& c) { target_->Clear(d2d(c, 1)); }

void Canvas::fillRect(const Rect& r, const Color& c) { target_->FillRectangle(rf(r), brush(c)); }

void Canvas::fillRound(const Rect& r, float radius, const Color& c) { target_->FillRoundedRectangle(rr(r, radius), brush(c)); }

void Canvas::fillRoundGradient(const Rect& r, float radius, const Color& from, const Color& to, bool horizontal) {
  if (isWine() && (from.a < 1 || to.a < 1 || alpha_.back() < 1)) return fillRound(r, radius, mix(from, to, 0.5f));
  D2D1_GRADIENT_STOP stops[2] = {{0, d2d(from, alpha_.back())}, {1, d2d(to, alpha_.back())}};
  ComPtr<ID2D1GradientStopCollection> coll;
  if (FAILED(target_->CreateGradientStopCollection(stops, 2, coll.GetAddressOf()))) return fillRound(r, radius, from);
  ComPtr<ID2D1LinearGradientBrush> g;
  D2D1_POINT_2F a = D2D1::Point2F(r.x, r.y), b = horizontal ? D2D1::Point2F(r.right(), r.y) : D2D1::Point2F(r.x, r.bottom());
  if (FAILED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll.Get(), g.GetAddressOf()))) return fillRound(r, radius, from);
  target_->FillRoundedRectangle(rr(r, radius), g.Get());
}

void Canvas::strokeRound(const Rect& r, float radius, const Color& c, float width) {
  Rect h = r.inset(width / 2, width / 2);
  target_->DrawRoundedRectangle(rr(h, std::max(0.0f, radius - width / 2)), brush(c), width);
}

void Canvas::strokeRoundGradient(const Rect& r, float radius, const Color& from, const Color& to, float width) {
  if (isWine()) return strokeRound(r, radius, mix(from, to, 0.35f), width);
  D2D1_GRADIENT_STOP stops[2] = {{0, d2d(from, alpha_.back())}, {1, d2d(to, alpha_.back())}};
  ComPtr<ID2D1GradientStopCollection> coll;
  ComPtr<ID2D1LinearGradientBrush> g;
  if (FAILED(target_->CreateGradientStopCollection(stops, 2, coll.GetAddressOf())) ||
      FAILED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x + r.w * 0.6f, r.bottom())), coll.Get(),
                                                g.GetAddressOf())))
    return strokeRound(r, radius, from, width);
  Rect h = r.inset(width / 2, width / 2);
  target_->DrawRoundedRectangle(rr(h, std::max(0.0f, radius - width / 2)), g.Get(), width);
}

void Canvas::fillRoundStops(const Rect& r, float radius, std::initializer_list<std::pair<float, Color>> list, bool horizontal) {
  if (isWine()) {
    Color avg{0, 0, 0, 0};
    for (const auto& s : list) avg = {avg.r + s.second.r, avg.g + s.second.g, avg.b + s.second.b, avg.a + s.second.a};
    float n = float(list.size());
    if (n > 0) fillRound(r, radius, {avg.r / n, avg.g / n, avg.b / n, avg.a / n});
    return;
  }
  std::vector<D2D1_GRADIENT_STOP> stops;
  for (const auto& s : list) stops.push_back({s.first, d2d(s.second, alpha_.back())});
  ComPtr<ID2D1GradientStopCollection> coll;
  ComPtr<ID2D1LinearGradientBrush> g;
  D2D1_POINT_2F a = D2D1::Point2F(r.x, r.y), b = horizontal ? D2D1::Point2F(r.right(), r.y) : D2D1::Point2F(r.x, r.bottom());
  if (stops.empty() || FAILED(target_->CreateGradientStopCollection(stops.data(), UINT32(stops.size()), coll.GetAddressOf())) ||
      FAILED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll.Get(), g.GetAddressOf())))
    return;
  target_->FillRoundedRectangle(rr(r, radius), g.Get());
}

void Canvas::fillCircle(float cx, float cy, float radius, const Color& c) { target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(c)); }

void Canvas::strokeCircle(float cx, float cy, float radius, const Color& c, float width) {
  target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(c), width, round_.Get());
}

void Canvas::line(float x1, float y1, float x2, float y2, const Color& c, float width) {
  target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush(c), width, round_.Get());
}

void Canvas::arc(float cx, float cy, float radius, float startDeg, float sweepDeg, const Color& c, float width) {
  // short straight pieces (every ~4°): smooth at these sizes, and drawn the same everywhere
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(factory_->CreatePathGeometry(path.GetAddressOf())) || FAILED(path->Open(sink.GetAddressOf()))) return;
  int steps = std::max(2, int(std::fabs(sweepDeg) / 4.0f));
  for (int i = 0; i <= steps; i++) {
    float a = (startDeg + sweepDeg * float(i) / float(steps)) * kPi / 180;
    D2D1_POINT_2F p = D2D1::Point2F(cx + radius * std::cos(a), cy + radius * std::sin(a));
    if (i == 0) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
    else sink->AddLine(p);
  }
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  target_->DrawGeometry(path.Get(), brush(c), width, round_.Get());
}

void Canvas::verticalGradient(const Rect& r, const Color& top, const Color& bottom) {
  if (isWine() && (top.a < 1 || bottom.a < 1 || alpha_.back() < 1)) return fillRect(r, mix(top, bottom, 0.5f));
  D2D1_GRADIENT_STOP stops[2] = {{0, d2d(top, alpha_.back())}, {1, d2d(bottom, alpha_.back())}};
  ComPtr<ID2D1GradientStopCollection> coll;
  ComPtr<ID2D1LinearGradientBrush> g;
  if (FAILED(target_->CreateGradientStopCollection(stops, 2, coll.GetAddressOf())) ||
      FAILED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.bottom())), coll.Get(), g.GetAddressOf())))
    return fillRect(r, top);
  target_->FillRectangle(rf(r), g.Get());
}

void Canvas::glow(float cx, float cy, float radius, const Color& c) {
  if (isWine()) { // rings of growing transparency
    const int steps = 14;
    for (int i = steps; i >= 1; i--) {
      float k = float(i) / steps;
      fillCircle(cx, cy, radius * k, withAlpha(c, 1.6f / steps * (1 - k * 0.85f)));
    }
    return;
  }
  D2D1_GRADIENT_STOP stops[2] = {{0, d2d(c, alpha_.back())}, {1, d2d(withAlpha(c, 0), alpha_.back())}};
  ComPtr<ID2D1GradientStopCollection> coll;
  ComPtr<ID2D1RadialGradientBrush> g;
  if (FAILED(target_->CreateGradientStopCollection(stops, 2, coll.GetAddressOf())) ||
      FAILED(target_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), radius, radius), coll.Get(), g.GetAddressOf())))
    return;
  target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), g.Get());
}

void Canvas::shadow(const Rect& r, float radius, float strength) {
  for (int i = 4; i >= 1; i--) {
    float g = float(i) * 2.5f;
    fillRound({r.x - g, r.y - g + 4, r.w + 2 * g, r.h + 2 * g}, radius + g, rgb(0x000000, 0.045f * strength));
  }
}

IDWriteTextFormat* Canvas::format(float size, int weight, bool mono) {
  int key = (int(size * 10) * 1000 + weight) * 2 + (mono ? 1 : 0);
  auto it = formats_.find(key);
  if (it != formats_.end()) return it->second.Get();
  ComPtr<IDWriteTextFormat> f;
  dwrite_->CreateTextFormat((mono ? mono_ : family_).c_str(), nullptr, DWRITE_FONT_WEIGHT(weight), DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"hu-hu",
                            f.GetAddressOf());
  formats_[key] = f;
  return f.Get();
}

float Canvas::text(const std::wstring& s, const Rect& r, float size, int weight, const Color& c, Align align, bool vcenter, bool wrap) {
  if (s.empty() || !dwrite_) return 0;
  IDWriteTextFormat* f = format(size, weight);
  if (!f) return 0;
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite_->CreateTextLayout(s.c_str(), UINT32(s.size()), f, std::max(1.0f, r.w), std::max(1.0f, r.h), layout.GetAddressOf()))) return 0;
  layout->SetTextAlignment(align == Center ? DWRITE_TEXT_ALIGNMENT_CENTER : align == Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
  layout->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  if (!wrap) {
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(f, ellipsis.GetAddressOf()))) {
      DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      layout->SetTrimming(&t, ellipsis.Get());
    }
  }
  target_->DrawTextLayout(D2D1::Point2F(r.x, r.y), layout.Get(), brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  return m.height;
}

float Canvas::measure(const std::wstring& s, float size, int weight, float maxWidth, float* height) {
  if (height) *height = 0;
  if (s.empty() || !dwrite_) return 0;
  IDWriteTextFormat* f = format(size, weight);
  ComPtr<IDWriteTextLayout> layout;
  if (!f || FAILED(dwrite_->CreateTextLayout(s.c_str(), UINT32(s.size()), f, maxWidth, 100000, layout.GetAddressOf()))) return 0;
  layout->SetWordWrapping(maxWidth < 99999 ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  if (height) *height = m.height;
  return m.widthIncludingTrailingWhitespace;
}

void Canvas::gradientText(const std::wstring& s, const Rect& r, float size, int weight, const Color& from, const Color& to) {
  IDWriteTextFormat* f = format(size, weight);
  ComPtr<IDWriteTextLayout> layout;
  if (!f || FAILED(dwrite_->CreateTextLayout(s.c_str(), UINT32(s.size()), f, r.w, r.h, layout.GetAddressOf()))) return;
  layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  D2D1_GRADIENT_STOP stops[2] = {{0, d2d(from, alpha_.back())}, {1, d2d(to, alpha_.back())}};
  ComPtr<ID2D1GradientStopCollection> coll;
  ComPtr<ID2D1LinearGradientBrush> g;
  if (FAILED(target_->CreateGradientStopCollection(stops, 2, coll.GetAddressOf())) ||
      FAILED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x + std::max(1.0f, m.width), r.y)), coll.Get(),
                                                g.GetAddressOf())))
    return;
  target_->DrawTextLayout(D2D1::Point2F(r.x, r.y), layout.Get(), g.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float Canvas::monoText(const std::wstring& s, const Rect& r, float size, int weight, const Color& c, Align align) {
  if (s.empty() || !dwrite_) return 0;
  IDWriteTextFormat* f = format(size, weight, true);
  ComPtr<IDWriteTextLayout> layout;
  if (!f || FAILED(dwrite_->CreateTextLayout(s.c_str(), UINT32(s.size()), f, std::max(1.0f, r.w), std::max(1.0f, r.h), layout.GetAddressOf()))) return 0;
  layout->SetTextAlignment(align == Center ? DWRITE_TEXT_ALIGNMENT_CENTER : align == Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
  layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  target_->DrawTextLayout(D2D1::Point2F(r.x, r.y), layout.Get(), brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  return m.width;
}

void Canvas::appIcon(const Rect& r) {
  // the icon at the size it shows in pixels: Windows picks the best image of the .ico and scales it
  int size = std::max(16, int(std::lround(r.w * dpi_ / 96.0f)));
  ComPtr<ID2D1Bitmap>& bitmap = icons_[size];
  if (!bitmap) {
    HICON icon = HICON(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICFormatConverter> premultiplied;
    if (icon && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHICON(icon, &source)) && SUCCEEDED(wic->CreateFormatConverter(&premultiplied)) &&
        SUCCEEDED(premultiplied->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
      target_->CreateBitmapFromWicBitmap(premultiplied.Get(), nullptr, bitmap.GetAddressOf());
    if (icon) DestroyIcon(icon);
    if (!bitmap) {
      icons_.erase(size);
      return;
    }
  }
  target_->DrawBitmap(bitmap.Get(), rf(r), std::clamp(alpha_.back(), 0.0f, 1.0f), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Canvas::icon(Icon i, float cx, float cy, float size, const Color& c, float stroke) {
  float s = size / 24.0f;
  auto X = [&](float x) { return cx + (x - 12) * s; };
  auto Y = [&](float y) { return cy + (y - 12) * s; };
  auto L = [&](float x1, float y1, float x2, float y2) { line(X(x1), Y(y1), X(x2), Y(y2), c, stroke); };
  auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts, bool fill, bool closed) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(path.GetAddressOf())) || FAILED(path->Open(sink.GetAddressOf()))) return;
    auto it = pts.begin();
    sink->BeginFigure(D2D1::Point2F(X(it->x), Y(it->y)), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (++it; it != pts.end(); ++it) sink->AddLine(D2D1::Point2F(X(it->x), Y(it->y)));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    if (fill) target_->FillGeometry(path.Get(), brush(c));
    else target_->DrawGeometry(path.Get(), brush(c), stroke, round_.Get());
  };
  auto box = [&](float x, float y, float w, float h, float r, bool fill) {
    Rect b{X(x), Y(y), w * s, h * s};
    if (fill) fillRound(b, r * s, c);
    else target_->DrawRoundedRectangle(rr(b, r * s), brush(c), stroke);
  };
  switch (i) {
    case Icon::Monitor:
      box(3, 4, 18, 12, 2, false);
      L(12, 16, 12, 20);
      L(8, 20, 16, 20);
      break;
    case Icon::Eye:
      target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 9.5f * s, 6 * s), brush(c), stroke);
      fillCircle(cx, cy, 2.6f * s, c);
      break;
    case Icon::Play: poly({{8, 5}, {19, 12}, {8, 19}}, true, true); break;
    case Icon::FullScreen:
      poly({{4, 9}, {4, 4}, {9, 4}}, false, false);
      poly({{15, 4}, {20, 4}, {20, 9}}, false, false);
      poly({{20, 15}, {20, 20}, {15, 20}}, false, false);
      poly({{9, 20}, {4, 20}, {4, 15}}, false, false);
      break;
    case Icon::Volume:
    case Icon::Sound:
    case Icon::Mute:
      poly({{3, 9}, {7, 9}, {12, 5}, {12, 19}, {7, 15}, {3, 15}}, true, true);
      if (i == Icon::Mute) {
        L(16, 9, 21, 15);
        L(21, 9, 16, 15);
      } else {
        arc(X(12), Y(12), 4.5f * s, -45, 90, c, stroke);
        arc(X(12), Y(12), 8 * s, -50, 100, c, stroke);
      }
      break;
    case Icon::Info:
      strokeCircle(cx, cy, 9.5f * s, c, stroke);
      fillCircle(X(12), Y(7.8f), 1.3f * s, c);
      L(12, 11, 12, 16.5f);
      break;
    case Icon::Copy:
      box(8, 8, 12, 12, 2.5f, false);
      poly({{5, 15}, {5, 6.5f}, {6.5f, 5}, {15, 5}}, false, false);
      break;
    case Icon::Check: poly({{5, 12.5f}, {10, 17.5f}, {19.5f, 7}}, false, false); break;
    case Icon::Back: poly({{14, 5}, {7, 12}, {14, 19}}, false, false); break;
    case Icon::Pause:
      box(6.5f, 5, 4, 14, 1.2f, true);
      box(13.5f, 5, 4, 14, 1.2f, true);
      break;
    case Icon::Stop: box(6, 6, 12, 12, 2.5f, true); break;
    case Icon::Download:
      L(12, 4, 12, 15);
      poly({{7, 10.5f}, {12, 15.5f}, {17, 10.5f}}, false, false);
      L(5, 20, 19, 20);
      break;
    case Icon::Link:
      box(2.5f, 8.5f, 11, 7, 3.5f, false);
      box(10.5f, 8.5f, 11, 7, 3.5f, false);
      break;
    case Icon::Users:
      strokeCircle(X(9), Y(8), 3.4f * s, c, stroke);
      arc(X(9), Y(20), 6 * s, 180, 180, c, stroke);
      strokeCircle(X(17), Y(9), 2.6f * s, c, stroke);
      arc(X(17.5f), Y(19.5f), 4.5f * s, 200, 140, c, stroke);
      break;
    case Icon::Lock:
      box(5, 11, 14, 9.5f, 2.2f, false);
      arc(X(12), Y(9.5f), 4 * s, 180, 180, c, stroke);
      L(8, 9.5f, 8, 11);
      L(16, 9.5f, 16, 11);
      break;
    case Icon::Close:
      L(6.5f, 6.5f, 17.5f, 17.5f);
      L(17.5f, 6.5f, 6.5f, 17.5f);
      break;
    case Icon::Refresh:
      arc(cx, cy, 7.5f * s, -80, 290, c, stroke);
      poly({{11.5f, 2}, {14.5f, 4.6f}, {11.2f, 7}}, false, false);
      break;
    case Icon::Bolt: poly({{13.5f, 2}, {5, 13.5f}, {11, 13.5f}, {10, 22}, {19, 10}, {13, 10}}, true, true); break;
  }
}

void Canvas::pushAlpha(float a) { alpha_.push_back(alpha_.back() * a); }
void Canvas::popAlpha() {
  if (alpha_.size() > 1) alpha_.pop_back();
}

void Canvas::applyTransform() { target_->SetTransform(transforms_.back()); }

void Canvas::pushTranslate(float dx, float dy) {
  transforms_.push_back(D2D1::Matrix3x2F::Translation(dx, dy) * transforms_.back());
  applyTransform();
}

void Canvas::popTranslate() {
  if (transforms_.size() > 1) transforms_.pop_back();
  applyTransform();
}

void Canvas::pushScale(float k, float cx, float cy) {
  transforms_.push_back(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(cx, cy)) * transforms_.back());
  applyTransform();
}

void Canvas::popScale() { popTranslate(); }

void Canvas::pushClip(const Rect& r) { target_->PushAxisAlignedClip(rf(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::popClip() { target_->PopAxisAlignedClip(); }

/* ------------------------------------------------ animations */

float Anims::to(const std::string& key, float target, float ms) {
  auto it = values_.find(key);
  if (it == values_.end()) {
    values_[key] = target;
    return target;
  }
  float& v = it->second;
  if (std::fabs(v - target) < 0.002f) {
    v = target;
    return v;
  }
  float k = 1.0f - std::exp(-float(dt_) / std::max(1.0f, ms / 3.0f));
  v += (target - v) * k;
  moving_ = true;
  return v;
}

void Anims::set(const std::string& key, float value) { values_[key] = value; }

float Anims::get(const std::string& key, float fallback) const {
  auto it = values_.find(key);
  return it == values_.end() ? fallback : it->second;
}

void Anims::frame(double nowMs) {
  dt_ = now_ > 0 ? std::clamp(nowMs - now_, 1.0, 50.0) : 16.0;
  now_ = nowMs;
  moving_ = false;
}

}  // namespace ss::ui
