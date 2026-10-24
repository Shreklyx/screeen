// The Windows app's look: everything but the text boxes and the video is drawn with Direct2D /
// DirectWrite (rounded cards, gradients, icons, a spinner), in device-independent pixels, and every
// change is animated – a value eases towards where it should be on each frame (Anims).
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <map>
#include <string>
#include <vector>

namespace ss::ui {

struct Color {
  float r = 0, g = 0, b = 0, a = 1;
};
Color rgb(uint32_t hex, float a = 1);
Color mix(const Color& a, const Color& b, float t);
Color withAlpha(const Color& c, float a);

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
  Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  float cx() const { return x + w / 2; }
  float cy() const { return y + h / 2; }
};

enum class Icon { Monitor, Eye, Play, FullScreen, Volume, Mute, Info, Copy, Check, Back, Pause, Stop, Download, Link, Users, Lock, Sound, Close, Refresh, Bolt };

enum Align { Left, Center, Right };

/** Fonts: the UI face (Segoe UI Variable on Windows 11, Segoe UI before). */
enum Weight { Regular = 400, Medium = 500, Semibold = 600, Bold = 700 };

class Canvas {
 public:
  Canvas();
  ~Canvas();
  /** Starts a frame for the window (creates / resizes the target as needed). */
  bool begin(HWND wnd, float dpi);
  /** Ends it; false when the target was lost (the next begin makes a new one). */
  void end();
  void resized();

  void clear(const Color& c);
  void fillRect(const Rect& r, const Color& c);
  void fillRound(const Rect& r, float radius, const Color& c);
  void fillRoundGradient(const Rect& r, float radius, const Color& from, const Color& to, bool horizontal = true);
  void strokeRound(const Rect& r, float radius, const Color& c, float width = 1);
  /** A border that catches the light: bright at the top left, fading towards the bottom right. */
  void strokeRoundGradient(const Rect& r, float radius, const Color& from, const Color& to, float width = 1);
  /** A rounded rectangle filled with a gradient of several stops (0..1). */
  void fillRoundStops(const Rect& r, float radius, std::initializer_list<std::pair<float, Color>> stops, bool horizontal = false);
  void fillCircle(float cx, float cy, float radius, const Color& c);
  void strokeCircle(float cx, float cy, float radius, const Color& c, float width = 1);
  void line(float x1, float y1, float x2, float y2, const Color& c, float width = 1.5f);
  void arc(float cx, float cy, float radius, float startDeg, float sweepDeg, const Color& c, float width);
  void verticalGradient(const Rect& r, const Color& top, const Color& bottom);
  void glow(float cx, float cy, float radius, const Color& c);
  void shadow(const Rect& r, float radius, float strength = 1);

  /** Draws text in r; returns the height it took. */
  float text(const std::wstring& s, const Rect& r, float size, int weight, const Color& c, Align align = Left, bool vcenter = true, bool wrap = false);
  /** The width (and height) the text needs. */
  float measure(const std::wstring& s, float size, int weight, float maxWidth = 100000, float* height = nullptr);
  /** Text filled with a horizontal gradient. */
  void gradientText(const std::wstring& s, const Rect& r, float size, int weight, const Color& from, const Color& to);

  void icon(Icon i, float cx, float cy, float size, const Color& c, float stroke = 1.7f);

  /** Everything drawn until pop is this much more transparent / moved / clipped. */
  void pushAlpha(float a);
  void popAlpha();
  void pushTranslate(float dx, float dy);
  void popTranslate();
  void pushClip(const Rect& r);
  void popClip();
  void pushScale(float s, float cx, float cy);
  void popScale();

  float width() const { return width_; }
  float height() const { return height_; }

 private:
  ID2D1SolidColorBrush* brush(const Color& c);
  IDWriteTextFormat* format(float size, int weight);
  void applyTransform();
  Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  std::map<int, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_;
  std::wstring family_;
  HWND wnd_ = nullptr;
  float dpi_ = 96, width_ = 0, height_ = 0;
  std::vector<float> alpha_{1.0f};
  std::vector<D2D1_MATRIX_3X2_F> transforms_;
};

/** Values that ease towards their targets, one per key; paint() asks, the frame loop goes on while any moves. */
class Anims {
 public:
  /** Exponential approach: about `ms` to get (almost) there. */
  float to(const std::string& key, float target, float ms = 140);
  /** Sets at once. */
  void set(const std::string& key, float value);
  float get(const std::string& key, float fallback = 0) const;
  /** Call at the start of each frame. */
  void frame(double nowMs);
  /** Something still moves (or wants frames: keepGoing()). */
  bool active() const { return moving_; }
  void keepGoing() { moving_ = true; }
  double now() const { return now_; }

 private:
  std::map<std::string, float> values_;
  double now_ = 0, dt_ = 16;
  bool moving_ = false;
};

/** 0..1 → 0..1, fast then gentle. */
inline float easeOut(float t) {
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  float u = 1 - t;
  return 1 - u * u * u;
}

}  // namespace ss::ui
