// Screen Share for Windows – native, without Electron or Chromium.
//
// Home: share the screen (sharing password, which screen, quality, system sound) or watch a room
// (code or link). Sharing: the room code and link, the viewers with their connection and latency.
// Watching: the picture fills the window (F / double click: full screen, S: statistics, M: mute).
// About: a small window of its own, as on a Mac – the icon, the name, the version (build), the
// copyright, updates. The server is always mozizas.hu.
//
// The look is drawn with Direct2D (ui.cpp) – cards, chips, switches, a spinner – and moves smoothly:
// screens cross-fade, buttons and switches ease. Only the text boxes are Windows controls, and the
// picture is the player's own swap chain.
//
// The session engine (native/core) does the networking; host.cpp captures and encodes,
// player.cpp / audio.cpp decode and play. Updates: update.cpp.
#include <windows.h>
#include <commctrl.h>
#include <dpapi.h>
#include <mfapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "capture.h"
#include "clock.h"
#include "engine.h"
#include "gpu.h"
#include "host.h"
#include "player.h"
#include "ui.h"
#include "update.h"

#ifndef SS_VERSION
#define SS_VERSION "2.0.0"
#endif
#ifndef SS_BUILD
#define SS_BUILD "0"
#endif
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

namespace ss {
std::unique_ptr<WebSocket> openWinHttpWebSocket(const std::string& url, WebSocketEvents events);
}

using namespace ss;
using namespace ss::ui;

namespace {

const UINT WM_ENGINE = WM_APP + 1; // wParam: session, lParam: std::string* (an event as JSON)
const UINT WM_UPDATE = WM_APP + 2; // wParam: 1 asked for; lParam: UpdateInfo* (nullptr: no newer version)
const UINT WM_UPDATE_PROGRESS = WM_APP + 3;
const UINT WM_UPDATE_DONE = WM_APP + 4; // wParam: ok

const UINT_PTR TIMER_TICK = 1, TIMER_ANIM = 3;

const char* kServer = "https://mozizas.hu";
const wchar_t* kSite = L"https://mozizas.hu";
const wchar_t* kVersion = WIDEN(SS_VERSION);
const wchar_t* kBuild = WIDEN(SS_BUILD);
const wchar_t* kCopyright = L"Copyright © 2026 mozizas.hu.\nMinden jog fenntartva.";
const UINT kSysAbout = 0x1010; // "About" in the window menu (Alt+Space)

// the palette
const Color cBg1 = rgb(0x0D0D18), cBg2 = rgb(0x06060B), cCard = rgb(0x1A1A26), cCardHi = rgb(0x242434), cBorder = rgb(0xFFFFFF, 0.09f),
            cText = rgb(0xEFEFF6), cMuted = rgb(0xA0A0B6), cFaint = rgb(0x6E6E86), cAccent = rgb(0xFF4D6A), cAccent2 = rgb(0x8B5CF6),
            cField = rgb(0x10101A), cOk = rgb(0x4ADE80), cWarn = rgb(0xFBBF24), cErr = rgb(0xFF6B7A), cWhite = rgb(0xFFFFFF);
const COLORREF kFieldGdi = RGB(0x10, 0x10, 0x1A), kTextGdi = RGB(0xEF, 0xEF, 0xF6), kBgGdi = RGB(0x0D, 0x0D, 0x18);

enum class Screen { Home, Hosting, Viewing };

// what can be clicked (and the text boxes)
enum Id {
  ID_NONE = 0,
  ID_PASSWORD = 100, ID_CODE,
  B_SHARE = 200, B_JOIN, B_REMEMBER, B_AUDIO, B_UPDATE, B_ABOUT, B_COPY, B_PAUSE, B_STOP, B_FULL, B_LEAVE, B_VOLUME, B_MUTE,
  B_CHECK, B_SITE, B_MODAL_OK, B_MODAL_CANCEL,
  B_MONITOR0 = 300, B_QUALITY0 = 400,
};

struct Quality {
  const wchar_t* name;  // on the chip
  const wchar_t* hint;  // under the chips
  int w, h, fps;
};
const Quality kQualities[] = {
    {L"Automatikus", L"A legszebb, amit a gép és a hálózat bír: akár 4K, 60 FPS.", 3840, 2160, 60},
    {L"1440p · 60", L"2560×1440, 60 FPS.", 2560, 1440, 60},
    {L"1080p · 60", L"1920×1080, 60 FPS.", 1920, 1080, 60},
    {L"1080p · 120", L"1920×1080, 120 FPS – játékhoz, gyors mozgáshoz.", 1920, 1080, 120},
    {L"720p · 60", L"1280×720, 60 FPS – gyenge internetre.", 1280, 720, 60},
    {L"Eredeti", L"A képernyő saját felbontása, 60 FPS.", 0, 0, 60},
};

struct Hit {
  Rect r;
  int id;
};

struct App {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr;
  UINT dpi = 96;
  HFONT editFont = nullptr;
  HBRUSH fieldBrush = nullptr;

  Screen screen = Screen::Home, prev = Screen::Home;
  bool transitioning = false;
  double transitionStart = 0;

  HWND pass = nullptr, code = nullptr, video = nullptr;
  std::map<int, Rect> fieldRects; // where the text boxes go (DIPs, in the scrolled content)
  std::map<int, RECT> placed;     // where they are (pixels)
  int focus = ID_NONE;

  Canvas canvas;
  Anims anim;
  bool animTimer = false;
  std::vector<Hit> hits;
  int hover = ID_NONE, pressed = ID_NONE;
  bool tracking = false, measuring = false;
  float scroll = 0, scrollTarget = 0, contentHeight = 0;

  std::vector<Monitor> monitors;
  int monitor = 0, quality = 0;
  bool remember = false, audio = true;

  std::unique_ptr<Engine> engine;
  std::unique_ptr<HostPipeline> host;
  std::unique_ptr<VideoPlayer> player;
  std::unique_ptr<AudioPlayer> audioOut;
  std::atomic<int> session{0};
  bool live = true, picture = false, fullscreen = false, stats = false, muted = false, draggingVolume = false;
  float volume = 1.0f;
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  LONG_PTR savedStyle = 0;

  std::wstring shareErr, joinErr, statusText, roomCode, info, overlayText, viewInfo;
  std::string statusKind, roomLink;
  Json viewers = Json::array(), viewStats;
  double copiedAt = -1e9;

  // a question or a note over the screen (instead of a system message box)
  struct Modal {
    bool open = false;
    std::wstring title, text, ok, cancel;
    bool danger = false;
    std::function<void()> action;
  } modal;

  UpdateInfo update;
  bool updating = false, checking = false;
  int updateProgress = -1;
  std::wstring updateNote;

  // the About window: its own canvas, animations and buttons; while it paints, the widgets draw there
  HWND about = nullptr;
  Canvas aboutCanvas;
  Anims aboutAnim;
  std::vector<Hit> aboutHits;
  int aboutHover = ID_NONE, aboutPressed = ID_NONE;
  bool aboutTimer = false, aboutTracking = false, paintingAbout = false;
};

App g;

int px(float dips) { return int(std::lround(dips * float(g.dpi) / 96.0f)); }
float dip(int pixels) { return float(pixels) * 96.0f / float(g.dpi); }

std::wstring toW(const std::string& s) { return widen(s); }

std::wstring getText(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(size_t(n) + 1, L'\0');
  GetWindowTextW(h, &s[0], n + 1);
  s.resize(size_t(n));
  return s;
}

void invalidate() { InvalidateRect(g.wnd, nullptr, FALSE); }

/** Frames keep coming while something moves. */
void animate() {
  if (!g.animTimer) {
    g.animTimer = true;
    SetTimer(g.wnd, TIMER_ANIM, 15, nullptr);
  }
  invalidate();
}

/** The About window shows the update state too: it is drawn anew when that changes. */
void refreshAbout() {
  if (g.about) InvalidateRect(g.about, nullptr, FALSE);
}

/* ------------------------------------------------ settings (HKCU\Software\ScreenShare) */

const wchar_t* kKey = L"Software\\ScreenShare";

DWORD loadDword(const wchar_t* name, DWORD fallback) {
  DWORD v = 0, size = sizeof v;
  return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : fallback;
}

void saveDword(const wchar_t* name, DWORD v) { RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &v, sizeof v); }

/** The sharing password, encrypted for this Windows user (DPAPI). */
void savePassword(const std::wstring& pw) {
  if (pw.empty()) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, L"Password");
    return;
  }
  DATA_BLOB in{DWORD(pw.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(pw.data()))}, out{};
  if (CryptProtectData(&in, L"Screen Share", nullptr, nullptr, nullptr, 0, &out)) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, L"Password", REG_BINARY, out.pbData, out.cbData);
    LocalFree(out.pbData);
  }
}

std::wstring loadPassword() {
  BYTE buf[4096];
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"Password", RRF_RT_REG_BINARY, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
  DATA_BLOB in{size, buf}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return L"";
  std::wstring pw(reinterpret_cast<wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
  LocalFree(out.pbData);
  return pw;
}

/* ------------------------------------------------ widgets (drawn; while measuring only laid out) */

Canvas& C() { return g.paintingAbout ? g.aboutCanvas : g.canvas; }
Anims& A() { return g.paintingAbout ? g.aboutAnim : g.anim; }

void hit(int id, const Rect& r) {
  if (g.paintingAbout) g.aboutHits.push_back({r, id});
  else if (!g.measuring && !g.transitioning) g.hits.push_back({r, id});
}

std::string key(const char* what, int id) { return std::string(what) + std::to_string(id); }

/** Frosted glass: a dark veil and a faint white sheen over the coloured light behind, a border
 *  that catches the light at the top left, a highlight along the top edge, a soft shadow. */
void glass(const Rect& r, float radius, float strength = 1, bool shadow = true) {
  Canvas& c = C();
  if (shadow) c.shadow(r, radius, 1.3f * strength);
  c.fillRound(r, radius, rgb(0x0A0A14, 0.48f));
  c.fillRoundStops(r, radius, {{0, rgb(0xFFFFFF, 0.10f * strength)}, {0.35f, rgb(0xFFFFFF, 0.05f * strength)}, {1, rgb(0xFFFFFF, 0.025f * strength)}});
  c.strokeRoundGradient(r, radius, rgb(0xFFFFFF, 0.30f * strength), rgb(0xFFFFFF, 0.05f), 1);
  float inset = std::max(radius, 10.0f);
  if (r.w > 2 * inset + 10)
    c.fillRoundStops({r.x + inset, r.y + 0.5f, r.w - 2 * inset, 1.2f}, 0.6f,
                     {{0, rgb(0xFFFFFF, 0)}, {0.5f, rgb(0xFFFFFF, 0.45f * strength)}, {1, rgb(0xFFFFFF, 0)}}, true);
}

enum class Style { Primary, Secondary, Danger, Ghost, Success };

void button(int id, const Rect& r, const std::wstring& label, Style style, bool hasIcon = false, Icon icon = Icon::Play, bool enabled = true) {
  if (g.measuring) return;
  if (enabled) hit(id, r);
  float hov = A().to(key("h", id), enabled && g.hover == id ? 1.0f : 0.0f, 130);
  float prs = A().to(key("p", id), enabled && g.pressed == id && g.hover == id ? 1.0f : 0.0f, 70);
  Canvas& c = C();
  c.pushScale(1.0f - 0.03f * prs, r.cx(), r.cy());
  float radius = std::min(12.0f, r.h / 2);
  Color text = cText;
  switch (style) {
    case Style::Primary:
      c.glow(r.cx(), r.bottom(), r.w * 0.55f, withAlpha(cAccent, 0.12f + 0.20f * hov));
      c.fillRoundGradient(r, radius, mix(cAccent, cWhite, 0.10f * hov), mix(cAccent2, cWhite, 0.10f * hov));
      // the gloss: light on the upper half
      c.fillRoundStops(r, radius, {{0, rgb(0xFFFFFF, 0.26f + 0.08f * hov)}, {0.5f, rgb(0xFFFFFF, 0.04f)}, {1, rgb(0xFFFFFF, 0)}});
      c.strokeRoundGradient(r, radius, rgb(0xFFFFFF, 0.55f), rgb(0xFFFFFF, 0.08f), 1);
      text = cWhite;
      break;
    case Style::Success:
      c.fillRound(r, radius, mix(rgb(0x123222, 0.85f), rgb(0x174A30, 0.9f), hov));
      c.strokeRoundGradient(r, radius, withAlpha(cOk, 0.7f), withAlpha(cOk, 0.2f), 1);
      text = cOk;
      break;
    case Style::Danger:
      c.fillRound(r, radius, mix(rgb(0x3A0F1C, 0.55f), rgb(0x5A1426, 0.7f), hov));
      c.fillRoundStops(r, radius, {{0, rgb(0xFFFFFF, 0.08f)}, {0.6f, rgb(0xFFFFFF, 0.0f)}});
      c.strokeRoundGradient(r, radius, withAlpha(cErr, 0.55f + 0.3f * hov), withAlpha(cErr, 0.12f), 1);
      text = rgb(0xFFC9D0);
      break;
    case Style::Secondary:
      c.fillRound(r, radius, rgb(0xFFFFFF, 0.07f + 0.06f * hov));
      c.fillRoundStops(r, radius, {{0, rgb(0xFFFFFF, 0.08f)}, {0.6f, rgb(0xFFFFFF, 0.0f)}});
      c.strokeRoundGradient(r, radius, rgb(0xFFFFFF, 0.28f + 0.12f * hov), rgb(0xFFFFFF, 0.05f), 1);
      break;
    case Style::Ghost:
      c.fillRound(r, radius, rgb(0xFFFFFF, 0.08f * hov));
      if (hov > 0.01f) c.strokeRoundGradient(r, radius, rgb(0xFFFFFF, 0.22f * hov), rgb(0xFFFFFF, 0.03f * hov), 1);
      text = mix(cMuted, cText, hov);
      break;
  }
  if (!enabled) text = withAlpha(text, 0.55f);
  float tw = label.empty() ? 0 : c.measure(label, 14, Semibold);
  float iw = hasIcon ? 18 : 0, gap = hasIcon && !label.empty() ? 9 : 0;
  float x = r.cx() - (tw + iw + gap) / 2;
  if (hasIcon) c.icon(icon, x + iw / 2, r.cy(), 18, text, 1.8f);
  if (!label.empty()) c.text(label, {x + iw + gap, r.y, tw + 4, r.h}, 14, Semibold, text);
  c.popScale();
}

void toggle(int id, const Rect& row, const std::wstring& label, bool on) {
  if (g.measuring) return;
  hit(id, row);
  Canvas& c = C();
  float k = A().to(key("t", id), on ? 1.0f : 0.0f, 170);
  float hov = A().to(key("h", id), g.hover == id ? 1.0f : 0.0f, 130);
  Rect track{row.x, row.cy() - 11, 40, 22};
  c.fillRound(track, 11, mix(rgb(0xFFFFFF, 0.10f + 0.05f * hov), mix(cAccent, cAccent2, 0.35f), k));
  c.strokeRoundGradient(track, 11, rgb(0xFFFFFF, 0.30f), rgb(0xFFFFFF, 0.05f), 1);
  float kx = track.x + 11 + k * 18;
  c.fillCircle(kx, track.cy() + 0.6f, 8.6f, rgb(0x000000, 0.25f));
  c.fillCircle(kx, track.cy(), 8, cWhite);
  c.text(label, {row.x + 52, row.y, row.w - 52, row.h}, 13.5f, Regular, mix(cMuted, cText, std::max(k, hov)));
}

void chip(int id, const Rect& r, const std::wstring& label, bool selected) {
  if (g.measuring) return;
  hit(id, r);
  Canvas& c = C();
  float s = A().to(key("s", id), selected ? 1.0f : 0.0f, 160);
  float hov = A().to(key("h", id), g.hover == id ? 1.0f : 0.0f, 120);
  c.fillRound(r, 10, mix(rgb(0xFFFFFF, 0.045f + 0.05f * hov), withAlpha(mix(cAccent, cAccent2, 0.3f), 0.26f), s));
  c.fillRoundStops(r, 10, {{0, rgb(0xFFFFFF, 0.07f)}, {0.6f, rgb(0xFFFFFF, 0)}});
  c.strokeRoundGradient(r, 10, mix(rgb(0xFFFFFF, 0.22f + 0.12f * hov), withAlpha(rgb(0xFF8DA0), 0.95f), s), mix(rgb(0xFFFFFF, 0.04f), withAlpha(cAccent2, 0.5f), s),
                        1 + 0.3f * s);
  c.text(label, r.inset(8, 0), 13, s > 0.5f ? Semibold : Medium, mix(cMuted, cText, std::max(s, hov * 0.7f)), Center);
}

/** A text box: drawn here, the Windows edit control sits inside it. */
void field(int id, const Rect& r, Icon icon) {
  if (!g.transitioning || !g.measuring) g.fieldRects[id] = r;
  if (g.measuring) return;
  Canvas& c = C();
  float f = A().to(key("f", id), g.focus == id ? 1.0f : 0.0f, 150);
  if (f > 0.01f) c.fillRound({r.x - 3, r.y - 3, r.w + 6, r.h + 6}, 13, withAlpha(cAccent, 0.16f * f));
  c.fillRound(r, 10, cField);
  c.strokeRoundGradient(r, 10, mix(rgb(0xFFFFFF, 0.16f), withAlpha(rgb(0xFF8DA0), 0.95f), f), mix(rgb(0xFFFFFF, 0.06f), withAlpha(cAccent2, 0.7f), f), 1 + 0.5f * f);
  c.icon(icon, r.x + 20, r.cy(), 16, mix(cFaint, cAccent, f), 1.6f);
  // while the screens cross-fade the edit control is hidden: its text is drawn instead
  HWND h = id == ID_PASSWORD ? g.pass : g.code;
  if (g.transitioning && h) {
    std::wstring t = getText(h);
    if (id == ID_PASSWORD) t = std::wstring(t.size(), L'\x25CF');
    c.text(t, {r.x + 40, r.y, r.w - 52, r.h}, 14, Regular, cText);
  }
}

void label(const std::wstring& s, const Rect& r) {
  if (!g.measuring) C().text(s, r, 12.5f, Medium, cMuted);
}

/** A card behind content laid out by fn(y) → its bottom. */
template <typename Fn>
float card(float x, float y, float w, float minH, Fn fn) {
  bool was = g.measuring;
  g.measuring = true;
  float bottom = fn(y);
  g.measuring = was;
  float h = std::max(minH, bottom - y);
  if (!g.measuring) {
    glass({x, y, w, h}, 20);
    fn(y);
  }
  return h;
}

void spinner(float cx, float cy, float radius, const Color& col) {
  if (g.measuring) return;
  Canvas& c = C();
  double t = A().now();
  float spin = float(std::fmod(t * 0.40, 360.0));
  float sweep = 70 + 110 * float(0.5 + 0.5 * std::sin(t / 420.0));
  c.strokeCircle(cx, cy, radius, withAlpha(col, 0.15f), 3);
  c.arc(cx, cy, radius, spin, sweep, col, 3);
  A().keepGoing();
}

void dot(float cx, float cy, const Color& col, float r = 4.5f) {
  if (g.measuring) return;
  C().fillCircle(cx, cy, r * 2.4f, withAlpha(col, 0.14f));
  C().fillCircle(cx, cy, r, col);
}

void logo(float x, float y, float size) {
  if (g.measuring) return;
  Canvas& c = C();
  Rect r{x, y, size, size};
  c.glow(r.cx(), r.cy(), size * 1.1f, withAlpha(cAccent, 0.16f));
  c.fillRoundGradient(r, size * 0.3f, cAccent, cAccent2);
  c.fillRoundStops(r, size * 0.3f, {{0, rgb(0xFFFFFF, 0.35f)}, {0.5f, rgb(0xFFFFFF, 0.05f)}, {1, rgb(0xFFFFFF, 0)}});
  c.strokeRoundGradient(r, size * 0.3f, rgb(0xFFFFFF, 0.6f), rgb(0xFFFFFF, 0.1f), 1);
  c.icon(Icon::Monitor, r.cx(), r.cy() - size * 0.02f, size * 0.56f, cWhite, std::max(1.8f, size / 20));
}

/* ------------------------------------------------ the screens (each returns its content's bottom) */

float homeShare(float x, float y, float w) {
  Canvas& c = C();
  float p = 24, cx = x + p, iw = w - 2 * p, cy = y + p;
  if (!g.measuring) {
    c.fillRound({cx, cy, 38, 38}, 11, withAlpha(cAccent, 0.15f));
    c.icon(Icon::Monitor, cx + 19, cy + 19, 20, cAccent, 1.8f);
    c.text(L"Képernyő megosztása", {cx + 52, cy - 1, iw - 52, 22}, 17, Semibold, cText);
    c.text(L"A képernyőd és a gép hangja – a lehető legkisebb késéssel.", {cx + 52, cy + 21, iw - 52, 18}, 12.5f, Regular, cMuted);
  }
  cy += 58;
  label(L"Megosztási jelszó", {cx, cy, iw, 18});
  cy += 24;
  field(ID_PASSWORD, {cx, cy, iw, 42}, Icon::Lock);
  cy += 52;
  toggle(B_REMEMBER, {cx, cy, iw, 26}, L"Jelszó megjegyzése", g.remember);
  cy += 40;
  label(L"Képernyő", {cx, cy, iw, 18});
  cy += 24;
  {
    float xx = cx, h = 36;
    for (size_t i = 0; i < g.monitors.size(); i++) {
      const std::wstring& name = g.monitors[i].name;
      float cw = std::min(iw, c.measure(name, 13, Semibold) + 32);
      if (xx > cx && xx + cw > cx + iw) {
        xx = cx;
        cy += h + 8;
      }
      chip(B_MONITOR0 + int(i), {xx, cy, cw, h}, name, int(i) == g.monitor);
      xx += cw + 8;
    }
    if (g.monitors.empty() && !g.measuring) c.text(L"Nem találok képernyőt.", {cx, cy, iw, h}, 13, Regular, cErr);
    cy += h + 18;
  }
  label(L"Minőség", {cx, cy, iw, 18});
  cy += 24;
  {
    int cols = iw > 420 ? 3 : 2;
    float gap = 8, cw = (iw - gap * float(cols - 1)) / float(cols), h = 36;
    int n = int(std::size(kQualities));
    for (int i = 0; i < n; i++) {
      int col = i % cols, rowi = i / cols;
      chip(B_QUALITY0 + i, {cx + float(col) * (cw + gap), cy + float(rowi) * (h + gap), cw, h}, kQualities[i].name, i == g.quality);
    }
    cy += float((n + cols - 1) / cols) * (h + gap);
    if (!g.measuring) c.text(kQualities[g.quality].hint, {cx, cy, iw, 18}, 12, Regular, cFaint);
    cy += 30;
  }
  toggle(B_AUDIO, {cx, cy, iw, 26}, L"A gép hangja is (5.1 / 7.1 térhang is)", g.audio);
  cy += 44;
  button(B_SHARE, {cx, cy, iw, 48}, L"Megosztás indítása", Style::Primary, true, Icon::Play);
  cy += 48;
  if (!g.shareErr.empty()) {
    float a = A().to("shareErr", 1, 220);
    if (!g.measuring) {
      c.pushAlpha(a);
      c.text(g.shareErr, {cx, cy + 12, iw, 40}, 13, Medium, cErr, Left, false, true);
      c.popAlpha();
    }
    cy += 40;
  }
  return cy + p;
}

float homeJoin(float x, float y, float w) {
  Canvas& c = C();
  float p = 24, cx = x + p, iw = w - 2 * p, cy = y + p;
  if (!g.measuring) {
    c.fillRound({cx, cy, 38, 38}, 11, withAlpha(cAccent2, 0.18f));
    c.icon(Icon::Eye, cx + 19, cy + 19, 20, rgb(0xB49CFF), 1.8f);
    c.text(L"Megosztás nézése", {cx + 52, cy - 1, iw - 52, 22}, 17, Semibold, cText);
    c.text(L"Írd be a szobakódot, vagy illeszd be a kapott linket.", {cx + 52, cy + 21, iw - 52, 18}, 12.5f, Regular, cMuted);
  }
  cy += 58;
  label(L"Szobakód vagy link", {cx, cy, iw, 18});
  cy += 24;
  field(ID_CODE, {cx, cy, iw, 42}, Icon::Link);
  cy += 56;
  button(B_JOIN, {cx, cy, iw, 48}, L"Csatlakozás", Style::Primary, true, Icon::Eye);
  cy += 48;
  if (!g.joinErr.empty()) {
    float a = A().to("joinErr", 1, 220);
    if (!g.measuring) {
      c.pushAlpha(a);
      c.text(g.joinErr, {cx, cy + 12, iw, 40}, 13, Medium, cErr, Left, false, true);
      c.popAlpha();
    }
    cy += 40;
  }
  cy += 18;
  if (!g.measuring) c.text(L"A megosztótól kapott 6 jelű kód, vagy a teljes link.", {cx, cy, iw, 18}, 12, Regular, cFaint);
  cy += 18;
  return cy + p;
}

float drawHome(float W, float) {
  Canvas& c = C();
  float m = 36, cw = std::min(W - 2 * m, 1060.0f), x0 = std::floor((W - cw) / 2), y = 30;
  // header
  logo(x0, y, 44);
  if (!g.measuring) {
    c.gradientText(L"Screen Share", {x0 + 58, y - 3, 400, 30}, 25, Bold, cText, rgb(0xD9D2FF));
    c.text(L"4K képernyőmegosztás hanggal · saját, titkosított átvitel", {x0 + 58, y + 25, cw - 200, 20}, 13, Regular, cMuted);
  }
  button(B_ABOUT, {x0 + cw - 124, y + 5, 124, 36}, L"Névjegy", Style::Ghost, true, Icon::Info);
  y += 78;
  // the two cards: side by side, or one under the other on a narrow window
  float gap = 20;
  bool wide = cw >= 700;
  float colW = wide ? (cw - gap) / 2 : cw;
  float hShare, hJoin;
  {
    bool was = g.measuring;
    g.measuring = true;
    hShare = homeShare(x0, y, colW) - y;
    hJoin = homeJoin(x0, y, colW) - y;
    g.measuring = was;
  }
  float h = wide ? std::max(hShare, hJoin) : 0;
  card(x0, y, colW, h, [&](float yy) { return homeShare(x0, yy, colW); });
  float y2 = wide ? y : y + hShare + gap;
  float x2 = wide ? x0 + colW + gap : x0;
  card(x2, y2, colW, h, [&](float yy) { return homeJoin(x2, yy, colW); });
  y = (wide ? y + h : y2 + hJoin) + 18;
  // footer: version, update
  if (!g.measuring) c.text(std::wstring(L"Screen Share ") + kVersion + L" · natív alkalmazás", {x0 + 4, y, 400, 36}, 12, Regular, cFaint);
  if (!g.update.version.empty()) {
    Rect r{x0 + cw - 230, y, 230, 36};
    if (g.updating && g.updateProgress >= 0 && !g.measuring) {
      float k = A().to("updateBar", float(g.updateProgress) / 100.0f, 250);
      c.fillRound(r, 12, cCardHi);
      c.fillRoundGradient({r.x, r.y, std::max(24.0f, r.w * k), r.h}, 12, cAccent, cAccent2);
      c.text(L"Letöltés… " + std::to_wstring(g.updateProgress) + L"%", r, 13.5f, Semibold, cWhite, Center);
    } else {
      button(B_UPDATE, r, g.updating ? L"Telepítés…" : L"Frissítés: " + toW(g.update.version), Style::Primary, true, Icon::Download, !g.updating);
    }
  }
  return y + 50;
}

std::wstring pathName(const std::string& path) {
  static const std::map<std::string, std::wstring> names = {
      {"lan", L"helyi hálózat"}, {"internet", L"interneten át"}, {"relay", L"a szerveren át"}, {"turn", L"TURN relén át"}, {"tcp", L"HTTPS-en át"}};
  auto it = names.find(path);
  return it == names.end() ? toW(path) : it->second;
}

Color statusColor() { return g.statusKind == "ok" ? cOk : g.statusKind == "warn" ? cWarn : g.statusKind == "error" ? cErr : cMuted; }

/** One viewer: status dot, name and path, the numbers as small pills. */
void viewerRow(const Json& v, const Rect& r) {
  Canvas& c = C();
  std::string id = v["id"].str(""), state = v["state"].str("");
  float a = A().to("v" + id, 1, 260);
  c.pushAlpha(a);
  c.pushTranslate(0, (1 - a) * 10);
  glass(r, 16, 0.85f, false);
  bool connected = state == "connected";
  Color col = connected ? cOk : state == "unsupported" ? cErr : state == "retrying" ? cWarn : cMuted;
  dot(r.x + 22, r.cy(), col, 4.5f);
  c.text(L"Néző #" + std::to_wstring(int(v["n"].num())), {r.x + 40, r.y + 9, 200, 20}, 14, Semibold, cText);
  std::wstring sub = connected ? L"közvetlen mód · " + pathName(v["path"].str(""))
                     : state == "unsupported" ? L"nem tud nézni: " + toW(v["whyText"].str(""))
                     : state == "retrying"    ? L"kapcsolat újra… " + toW(v["whyText"].str(""))
                                              : L"kapcsolódik…";
  c.text(sub, {r.x + 40, r.y + 29, r.w * 0.45f, 18}, 12, Regular, cMuted);
  if (connected) {
    std::vector<std::pair<std::wstring, Color>> pills;
    char b[48];
    if (v["fps"].isNumber()) pills.push_back({std::to_wstring(int(v["fps"].num())) + L" FPS", cText});
    if (v["kbps"].isNumber()) {
      std::snprintf(b, sizeof b, "%.1f Mbit/s", v["kbps"].num() / 1000);
      pills.push_back({toW(b), cText});
    }
    if (v["rtt"].isNumber()) pills.push_back({L"RTT " + std::to_wstring(int(std::lround(v["rtt"].num()))) + L" ms", cText});
    if (v["lat"].isNumber()) {
      double lat = v["lat"].num();
      pills.push_back({L"késés ~" + std::to_wstring(int(std::lround(lat))) + L" ms", lat < 40 ? cOk : lat < 90 ? cWarn : cErr});
    }
    float x = r.right() - 14;
    for (auto it = pills.rbegin(); it != pills.rend(); ++it) {
      float pw = c.measure(it->first, 12, Semibold) + 20;
      x -= pw;
      Rect pr{x, r.cy() - 13, pw, 26};
      c.fillRound(pr, 13, rgb(0xFFFFFF, 0.05f));
      c.text(it->first, pr, 12, Semibold, it->second, Center);
      x -= 6;
    }
  }
  c.popTranslate();
  c.popAlpha();
}

float drawHosting(float W, float) {
  Canvas& c = C();
  float m = 36, cw = std::min(W - 2 * m, 1000.0f), x0 = std::floor((W - cw) / 2), y = 30;
  if (!g.measuring) {
    c.text(L"Képernyőmegosztás", {x0, y, 360, 40}, 23, Bold, cText);
    float tw = c.measure(L"Képernyőmegosztás", 23, Bold);
    Rect badge{x0 + tw + 14, y + 8, g.live ? 64.0f : 96.0f, 24};
    c.fillRound(badge, 12, g.live ? withAlpha(cAccent, 0.18f) : rgb(0xFFFFFF, 0.08f));
    dot(badge.x + 14, badge.cy(), g.live ? cAccent : cMuted, 3.5f);
    c.text(g.live ? L"ÉLŐ" : L"SZÜNETEL", {badge.x + 24, badge.y, badge.w - 28, badge.h}, 11.5f, Bold, g.live ? rgb(0xFFB3BF) : cMuted);
  }
  button(B_STOP, {x0 + cw - 190, y, 190, 40}, L"Megosztás vége", Style::Danger, true, Icon::Stop);
  button(B_PAUSE, {x0 + cw - 190 - 12 - 150, y, 150, 40}, g.live ? L"Szünet" : L"Folytatás", Style::Secondary, true, g.live ? Icon::Pause : Icon::Play);
  y += 62;
  float h = card(x0, y, cw, 0, [&](float yy) {
    float p = 26, cx = x0 + p, iw = cw - 2 * p, cy = yy + p;
    if (!g.measuring) {
      c.text(L"SZOBAKÓD", {cx, cy, 200, 16}, 11.5f, Bold, cFaint);
      // the code, letter by letter, in the accent gradient
      std::wstring code = g.roomCode.empty() ? L"······" : g.roomCode;
      float xx = cx;
      for (wchar_t ch : code) {
        std::wstring s(1, ch);
        c.gradientText(s, {xx, cy + 18, 60, 58}, 44, Bold, cAccent, rgb(0xC08BFF));
        xx += c.measure(s, 44, Bold) + 6;
      }
    }
    cy += 86;
    float bw = 186;
    Rect lr{cx, cy, iw - bw - 12, 46};
    if (!g.measuring) {
      c.fillRound(lr, 12, cField);
      c.strokeRoundGradient(lr, 12, rgb(0xFFFFFF, 0.16f), rgb(0xFFFFFF, 0.06f), 1);
      c.icon(Icon::Link, lr.x + 22, lr.cy(), 16, cFaint, 1.6f);
      c.text(g.roomLink.empty() ? L"A link a kapcsolódás után jelenik meg…" : toW(g.roomLink), {lr.x + 42, lr.y, lr.w - 56, lr.h}, 14, Regular,
             g.roomLink.empty() ? cFaint : cText);
    }
    bool copied = A().now() - g.copiedAt < 2000;
    if (copied) A().keepGoing();
    button(B_COPY, {cx + iw - bw, cy, bw, 46}, copied ? L"Kimásolva" : L"Link másolása", copied ? Style::Success : Style::Primary, true, copied ? Icon::Check : Icon::Copy,
           !g.roomLink.empty());
    cy += 62;
    if (!g.measuring) {
      if (g.statusKind.empty()) spinner(cx + 8, cy + 11, 7, cMuted);
      else dot(cx + 8, cy + 11, statusColor(), 4.5f);
      c.text(g.statusText, {cx + 26, cy, iw - 26, 22}, 13.5f, Medium, g.statusKind.empty() ? cText : statusColor());
      c.text(g.info, {cx + 26, cy + 24, iw - 26, 20}, 12.5f, Regular, cMuted);
    }
    cy += 46;
    return cy + p - 6;
  });
  y += h + 26;
  int count = int(g.viewers.size());
  if (!g.measuring) {
    c.icon(Icon::Users, x0 + 12, y + 13, 20, cMuted, 1.7f);
    c.text(L"Nézők", {x0 + 32, y, 200, 26}, 16, Semibold, cText);
    float tw = c.measure(L"Nézők", 16, Semibold);
    Rect badge{x0 + 32 + tw + 10, y + 2, 28, 22};
    c.fillRound(badge, 11, count ? withAlpha(cAccent, 0.2f) : rgb(0xFFFFFF, 0.07f));
    c.text(std::to_wstring(count), badge, 12, Bold, count ? rgb(0xFFB3BF) : cMuted, Center);
  }
  y += 38;
  if (count == 0) {
    if (!g.measuring) {
      Rect r{x0, y, cw, 116};
      glass(r, 18, 0.6f, false);
      c.icon(Icon::Users, r.cx(), r.y + 40, 30, cFaint, 1.8f);
      c.text(L"Még nincs néző. Küldd el a linket vagy a kódot!", {r.x, r.y + 62, r.w, 30}, 14, Medium, cMuted, Center);
    }
    y += 116;
  } else {
    for (const Json& v : g.viewers.items()) {
      if (!g.measuring) viewerRow(v, {x0, y, cw, 58});
      y += 66;
    }
  }
  return y + 30;
}

const float kBarH = 56;

/** The viewer's bar: where its parts are (the drawing and the volume drag use the same numbers). */
struct BarLayout {
  Rect leave, full, volume, mute;
  float textRight;
};

BarLayout barLayout(float W) {
  BarLayout b;
  float right = W - 14;
  b.leave = {right - 118, 10, 118, 36};
  right -= 118 + 8;
  b.full = {right - 40, 10, 40, 36};
  right -= 40 + 18;
  b.volume = {right - 120, kBarH / 2 - 3, 120, 6};
  right -= 120 + 10;
  b.mute = {right - 38, 10, 38, 36};
  b.textRight = right - 38 - 10;
  return b;
}

float drawViewing(float W, float H) {
  Canvas& c = C();
  float barH = g.fullscreen ? 0 : kBarH;
  if (g.measuring) return H;
  c.fillRect({0, barH, W, H - barH}, rgb(0x000000));
  if (!g.fullscreen) {
    BarLayout b = barLayout(W);
    c.verticalGradient({0, 0, W, barH}, rgb(0x171726), rgb(0x10101A));
    c.glow(W * 0.1f, 0, 260, withAlpha(cAccent, 0.16f));
    c.glow(W * 0.9f, 0, 300, withAlpha(cAccent2, 0.16f));
    c.fillRect({0, 0, W, barH}, rgb(0xFFFFFF, 0.03f));
    c.fillRoundStops({0, barH - 1, W, 1}, 0, {{0, rgb(0xFFFFFF, 0)}, {0.5f, rgb(0xFFFFFF, 0.22f)}, {1, rgb(0xFFFFFF, 0)}}, true);
    dot(22, barH / 2, g.picture ? cOk : cWarn, 4.5f);
    button(B_LEAVE, b.leave, L"Kilépés", Style::Danger, true, Icon::Close);
    button(B_FULL, b.full, L"", Style::Ghost, true, Icon::FullScreen);
    // volume: the icon mutes, the bar sets it
    hit(B_VOLUME, {b.volume.x - 8, 12, b.volume.w + 16, 32});
    float vol = A().to("vol", g.muted ? 0.0f : g.volume, 90);
    float vh = A().to(key("h", B_VOLUME), g.hover == B_VOLUME || g.draggingVolume ? 1.0f : 0.0f, 120);
    c.fillRound(b.volume, 3, rgb(0xFFFFFF, 0.12f));
    c.fillRoundGradient({b.volume.x, b.volume.y, std::max(6.0f, b.volume.w * vol), b.volume.h}, 3, cAccent, cAccent2);
    c.fillCircle(b.volume.x + b.volume.w * vol, b.volume.cy(), 6 + 1.5f * vh, cWhite);
    button(B_MUTE, b.mute, L"", Style::Ghost, true, g.muted ? Icon::Mute : Icon::Volume);
    std::wstring head = L"Szoba: " + g.roomCode;
    c.text(head, {40, 0, 220, barH}, 14.5f, Semibold, cText);
    float hw = c.measure(head, 14.5f, Semibold);
    c.text(g.viewInfo, {40 + hw + 14, 0, std::max(0.0f, b.textRight - (40 + hw + 14)), barH}, 12.5f, Regular, cMuted);
  }
  if (!g.picture || g.transitioning || !g.player || !g.player->hasPicture()) {
    float cy = barH + (H - barH) / 2;
    spinner(W / 2, cy - 30, 18, cAccent);
    c.text(g.overlayText, {40, cy + 2, W - 80, 60}, 15, Medium, cText, Center, false, true);
  }
  return H;
}

/** The About window, like a Mac's: the icon, the name, the version (build), the copyright; updates
 *  and the site under them. */
void drawAboutWindow(float W, float H) {
  Canvas& c = C();
  c.verticalGradient({0, 0, W, H}, cBg1, cBg2);
  c.glow(W * 0.18f, H * 0.05f, 240, withAlpha(cAccent, 0.34f));
  c.glow(W * 0.92f, H * 0.42f, 260, withAlpha(cAccent2, 0.30f));
  float y = 28;
  logo(std::floor(W / 2 - 38), y, 76);
  y += 76 + 18;
  c.text(L"Screen Share", {0, y, W, 26}, 17, Bold, cText, Center);
  y += 27;
  c.text(std::wstring(L"Verzió ") + kVersion + L" (" + kBuild + L")", {0, y, W, 18}, 12, Regular, cMuted, Center);
  y += 30;
  float th = 0;
  c.measure(kCopyright, 11.5f, Regular, W - 56, &th);
  c.text(kCopyright, {28, y, W - 56, th + 4}, 11.5f, Regular, cFaint, Center, false, true);
  y += th + 22;
  Rect r{std::floor(W / 2 - 112), y, 224, 36};
  if (g.updating && g.updateProgress >= 0) {
    float k = A().to("updateBar", float(g.updateProgress) / 100.0f, 250);
    c.fillRound(r, 12, cCardHi);
    c.fillRoundGradient({r.x, r.y, std::max(24.0f, r.w * k), r.h}, 12, cAccent, cAccent2);
    c.text(L"Letöltés… " + std::to_wstring(g.updateProgress) + L"%", r, 13, Semibold, cWhite, Center);
  } else if (!g.update.version.empty()) {
    button(B_UPDATE, r, g.updating ? L"Telepítés…" : L"Frissítés: " + toW(g.update.version), Style::Primary, true, Icon::Download, !g.updating);
  } else {
    button(B_CHECK, r, g.checking ? L"Keresés…" : L"Frissítések keresése", Style::Secondary, true, Icon::Refresh, !g.checking);
  }
  y += 36 + 8;
  c.text(g.updateNote, {0, y, W, 18}, 12, Regular, g.update.version.empty() ? cMuted : cOk, Center);
  y += 24;
  button(B_SITE, {std::floor(W / 2 - 74), y, 148, 30}, L"mozizas.hu", Style::Ghost, true, Icon::Link);
  y += 40;
  const wchar_t* credits = L"BearSSL (MIT) · Opus (BSD) · NVIDIA Video Codec SDK (MIT)";
  c.measure(credits, 10.5f, Regular, W - 40, &th);
  c.text(credits, {20, y, W - 40, th + 4}, 10.5f, Regular, cFaint, Center, false, true);
}

void drawModal(float W, float H) {
  Canvas& c = C();
  float k = A().to("modal", g.modal.open ? 1.0f : 0.0f, 170);
  if (k < 0.005f) return;
  if (g.modal.open) g.hits.clear(); // only the dialog answers
  c.pushAlpha(k);
  c.fillRect({0, 0, W, H}, rgb(0x05050A, 0.62f));
  float w = std::min(440.0f, W - 48), th = 0;
  c.measure(g.modal.text, 13.5f, Regular, w - 56, &th);
  float h = 56 + 26 + th + 30 + 42 + 28;
  Rect r{std::floor((W - w) / 2), std::floor((H - h) / 2), w, h};
  c.pushScale(0.95f + 0.05f * k, r.cx(), r.cy());
  c.fillRound(r, 22, rgb(0x14141F, 0.80f));
  glass(r, 22, 1.4f);
  float x = r.x + 28, y = r.y + 28;
  c.text(g.modal.title, {x, y, w - 56, 26}, 18, Semibold, cText);
  y += 38;
  c.text(g.modal.text, {x, y, w - 56, th + 4}, 13.5f, Regular, cMuted, Left, false, true);
  y += th + 30;
  float bw = (w - 56 - 12) / 2;
  if (g.modal.cancel.empty()) {
    button(B_MODAL_OK, {x, y, w - 56, 42}, g.modal.ok, Style::Primary);
  } else {
    button(B_MODAL_CANCEL, {x, y, bw, 42}, g.modal.cancel, Style::Secondary);
    button(B_MODAL_OK, {x + bw + 12, y, bw, 42}, g.modal.ok, g.modal.danger ? Style::Danger : Style::Primary);
  }
  c.popScale();
  c.popAlpha();
}

void ask(const std::wstring& title, const std::wstring& text, const std::wstring& ok, const std::wstring& cancel, bool danger, std::function<void()> action) {
  if (!g.modal.open) A().set("modal", 0); // it comes up from nothing
  g.modal.open = true;
  g.modal.title = title;
  g.modal.text = text;
  g.modal.ok = ok;
  g.modal.cancel = cancel;
  g.modal.danger = danger;
  g.modal.action = std::move(action);
  g.hover = g.pressed = ID_NONE;
  SetFocus(g.wnd);
  InvalidateRect(g.wnd, nullptr, FALSE);
}

void closeModal(bool confirmed) {
  if (!g.modal.open) return;
  g.modal.open = false;
  std::function<void()> action = std::move(g.modal.action);
  g.modal.action = nullptr;
  if (confirmed && action) action();
  InvalidateRect(g.wnd, nullptr, FALSE);
}

float drawScreen(Screen s, float W, float H) {
  switch (s) {
    case Screen::Home: return drawHome(W, H);
    case Screen::Hosting: return drawHosting(W, H);
    case Screen::Viewing: return drawViewing(W, H);
  }
  return H;
}

/* ------------------------------------------------ the Windows controls: where they are, when shown */

void placeChild(int id, HWND h, bool visible) {
  RECT want = {0, 0, 0, 0};
  auto it = g.fieldRects.find(id);
  if (visible && it != g.fieldRects.end()) {
    const Rect& r = it->second;
    TEXTMETRICW tm{};
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, g.editFont);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    int x = px(r.x + 40), w = px(r.w - 52), eh = tm.tmHeight + 2;
    int y = px(r.y - g.scroll + r.h / 2) - eh / 2;
    want = {x, y, x + w, y + eh};
  }
  RECT& was = g.placed[id];
  if (visible && !EqualRect(&was, &want)) {
    was = want;
    SetWindowPos(h, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if ((IsWindowVisible(h) != FALSE) != visible) ShowWindow(h, visible ? SW_SHOWNA : SW_HIDE);
}

void updateChildren() {
  bool home = g.screen == Screen::Home && !g.transitioning;
  placeChild(ID_PASSWORD, g.pass, home);
  placeChild(ID_CODE, g.code, home);
  // (the window only once a frame is on it: before that a new swap chain shows whatever was there)
  bool video = g.screen == Screen::Viewing && g.picture && !g.transitioning && g.player && g.player->hasPicture();
  if (video) {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    int top = g.fullscreen ? 0 : px(kBarH);
    RECT want = {0, top, rc.right, std::max<LONG>(top + 1, rc.bottom)};
    RECT& was = g.placed[-1];
    if (!EqualRect(&was, &want)) {
      was = want;
      SetWindowPos(g.video, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_NOACTIVATE);
      if (g.player) g.player->resized();
    }
  }
  if ((IsWindowVisible(g.video) != FALSE) != video) ShowWindow(g.video, video ? SW_SHOWNA : SW_HIDE);
}

/* ------------------------------------------------ painting */

void paint() {
  Canvas& c = C();
  if (!c.begin(g.wnd, float(g.dpi))) return;
  A().frame(nowMs());
  g.hits.clear();
  float W = c.width(), H = c.height();
  float t = 1;
  if (g.transitioning) {
    t = easeOut(float((A().now() - g.transitionStart) / 240.0));
    if (t >= 1.0f) g.transitioning = false;
  }
  // the background: dark, with two soft glows
  if (g.screen == Screen::Viewing && !g.transitioning) {
    c.clear(rgb(0x000000));
  } else {
    c.verticalGradient({0, 0, W, H}, cBg1, cBg2);
    c.glow(W * 0.12f, H * 0.16f, 520, withAlpha(cAccent, 0.46f));
    c.glow(W * 0.90f, H * 0.30f, 560, withAlpha(cAccent2, 0.44f));
    c.glow(W * 0.52f, H * 1.05f, 600, withAlpha(rgb(0x3B82F6), 0.36f));
    c.glow(W * 0.00f, H * 0.86f, 360, withAlpha(rgb(0x22D3EE), 0.18f));
  }
  if (g.transitioning) {
    // the screens cross-fade: the old one rises and fades, the new one comes up
    c.pushAlpha(1 - t);
    c.pushTranslate(0, -14 * t);
    drawScreen(g.prev, W, H);
    c.popTranslate();
    c.popAlpha();
    c.pushAlpha(t);
    c.pushTranslate(0, 18 * (1 - t));
    g.contentHeight = drawScreen(g.screen, W, H);
    c.popTranslate();
    c.popAlpha();
    A().keepGoing();
  } else {
    // the scroll position: eased, kept within the content
    float maxScroll = std::max(0.0f, g.contentHeight - H);
    g.scrollTarget = std::clamp(g.scrollTarget, 0.0f, maxScroll);
    g.scroll = A().to("scroll", g.scrollTarget, 120);
    c.pushTranslate(0, -g.scroll);
    g.contentHeight = drawScreen(g.screen, W, H);
    c.popTranslate();
    if (g.screen != Screen::Viewing)
      for (Hit& hh : g.hits) hh.r.y -= g.scroll;
  }
  drawModal(W, H);
  if (g.screen == Screen::Viewing && g.picture && g.player && !g.player->hasPicture()) A().keepGoing(); // until the first frame shows
  c.end();
  updateChildren();
  if (A().active()) {
    animate();
  } else if (g.animTimer) {
    KillTimer(g.wnd, TIMER_ANIM);
    g.animTimer = false;
  }
}

void show(Screen s) {
  if (s == g.screen) return;
  g.prev = g.screen;
  g.screen = s;
  g.transitioning = true;
  g.transitionStart = nowMs();
  g.scrollTarget = 0;
  A().set("scroll", 0);
  g.scroll = 0;
  g.hover = g.pressed = ID_NONE;
  updateChildren(); // the text boxes and the picture hide while the screens cross-fade
  if (s != Screen::Home) SetFocus(g.wnd);
  animate();
}

/* ------------------------------------------------ sessions */

EngineCallbacks callbacks(int session) {
  EngineCallbacks cb;
  HWND wnd = g.wnd;
  cb.event = [wnd, session](const Json& e) { PostMessageW(wnd, WM_ENGINE, WPARAM(session), LPARAM(new std::string(e.dump()))); };
  return cb;
}

void endSession() {
  g.session++;
  if (g.host) g.host->stop(); // no more frames to the engine
  g.engine.reset();           // says goodbye to the server; no more callbacks after this
  g.host.reset();
  g.player.reset();
  g.audioOut.reset();
  g.picture = false;
}

void setError(std::wstring& where, const std::wstring& text, const char* animKey) {
  where = text;
  A().set(animKey, 0);
  animate();
}

void startSharing() {
  std::wstring pw = getText(g.pass);
  if (pw.empty()) {
    setError(g.shareErr, L"Add meg a megosztási jelszót.", "shareErr");
    SetFocus(g.pass);
    return;
  }
  if (g.monitor < 0 || g.monitor >= int(g.monitors.size())) {
    setError(g.shareErr, L"Nem találok képernyőt.", "shareErr");
    return;
  }
  saveDword(L"Quality", DWORD(g.quality));
  saveDword(L"Monitor", DWORD(g.monitor));
  saveDword(L"Audio", g.audio);
  savePassword(g.remember ? pw : L"");
  g.shareErr.clear();
  endSession();
  int session = ++g.session;
  EngineConfig cfg;
  cfg.server = kServer;
  cfg.webSocket = openWinHttpWebSocket;
  EngineCallbacks cb = callbacks(session);
  // the pipeline exists before the engine can call it
  auto host = std::make_shared<HostPipeline*>(nullptr);
  cb.streaming = [host](bool on) { if (*host) (*host)->setStreaming(on); };
  cb.keyframe = [host] { if (*host) (*host)->keyframe(); };
  cb.bitrate = [host](double bps) { if (*host) (*host)->setBitrate(bps); };
  g.engine.reset(new Engine(cfg, cb));
  g.host.reset(new HostPipeline(*g.engine));
  *host = g.host.get();
  const Quality& q = kQualities[g.quality];
  HostPipeline::Options o;
  o.monitor = g.monitors[size_t(g.monitor)].rect;
  o.maxWidth = q.w;
  o.maxHeight = q.h;
  o.fps = q.fps;
  o.audio = g.audio;
  g.host->start(o);
  g.live = true;
  g.viewers = Json::array();
  g.roomCode.clear();
  g.roomLink.clear();
  g.statusText = L"Kapcsolódás a szerverhez…";
  g.statusKind.clear();
  g.info.clear();
  show(Screen::Hosting);
  g.engine->host(narrow(pw.c_str()));
}

std::string parseCode(std::wstring s) {
  size_t hash = s.find(L'#');
  if (hash != std::wstring::npos) s = s.substr(hash + 1);
  std::string out;
  for (wchar_t c : s) {
    if (c >= L'A' && c <= L'Z') out += char(c - L'A' + 'a');
    else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) out += char(c);
  }
  return out;
}

void startWatching(const std::string& code) {
  if (code.size() < 4) {
    setError(g.joinErr, L"Érvénytelen szobakód – írd be a 6 jelű kódot, vagy illeszd be a teljes linket.", "joinErr");
    SetFocus(g.code);
    return;
  }
  g.joinErr.clear();
  endSession();
  int session = ++g.session;
  EngineConfig cfg;
  cfg.server = kServer;
  cfg.webSocket = openWinHttpWebSocket;
  g.player.reset(new VideoPlayer(g.video));
  g.audioOut.reset(new AudioPlayer());
  g.audioOut->setVolume(g.muted ? 0.0f : g.volume);
  EngineCallbacks cb = callbacks(session);
  VideoPlayer* player = g.player.get();
  AudioPlayer* audio = g.audioOut.get();
  cb.media = [player, audio](const MediaMeta& m, const uint8_t* d, size_t n) {
    if (m.stream == 0) player->push(d, n, m.key, m.capture);
    else audio->push(m.sub, d, n, m.ts);
  };
  g.engine.reset(new Engine(cfg, cb));
  Engine* engine = g.engine.get();
  player->clockOffset = [engine] { return engine->clockOffset(); };
  player->needKeyframe = [engine] { engine->requestKeyframe(); };
  g.picture = false;
  g.overlayText = L"Csatlakozás a szobához…";
  g.roomCode = toW(code);
  g.viewInfo.clear();
  show(Screen::Viewing);
  engine->join(code);
}

void toggleFullscreen();

void stopToHome(const std::wstring& message = L"") {
  if (g.fullscreen) toggleFullscreen();
  endSession();
  show(Screen::Home);
  if (!message.empty()) setError(g.joinErr, message, "joinErr");
}

/* ------------------------------------------------ engine events (UI thread) */

void onEngineEvent(const Json& e) {
  const std::string& type = e["type"].str();
  if (type == "status") {
    g.statusText = toW(e["text"].str());
    g.statusKind = e["kind"].str();
  } else if (type == "hosted") {
    g.roomCode = toW(e["code"].str());
    g.roomLink = e["link"].str();
    if (e["changed"].b()) ask(L"Új link készült", L"A korábbi szoba lejárt, ezért új kód és link készült – küldd el újra a nézőknek.", L"Rendben", L"", false, nullptr);
  } else if (type == "host-error") {
    std::string c = e["code"].str();
    std::wstring text = c == "auth" ? L"Hibás megosztási jelszó." : c == "rate" ? L"Túl sok próbálkozás, várj egy kicsit." : c == "full" ? L"A szerver megtelt, próbáld később."
                                                                                                                                        : L"Nem érem el a szervert (" + toW(c) + L").";
    endSession();
    show(Screen::Home);
    setError(g.shareErr, text, "shareErr");
  } else if (type == "viewers") {
    for (const Json& v : e["list"].items()) {
      std::string id = "v" + v["id"].str("");
      if (A().get(id, -1) < 0) A().set(id, 0); // a new viewer fades in
    }
    std::vector<Json> list = e["list"].items();
    std::sort(list.begin(), list.end(), [](const Json& a, const Json& b) { return a["n"].num() < b["n"].num(); });
    g.viewers = Json(list);
  } else if (type == "closed" && g.screen == Screen::Hosting) {
    stopToHome(L"A szobát egy másik ablak vette át.");
  } else if (type == "view-state" && g.screen == Screen::Viewing) {
    g.picture = e["picture"].b();
    if (!g.picture) g.overlayText = toW(e["text"].str());
  } else if (type == "view-stats") {
    g.viewStats = e;
  } else if (type == "closed" && g.screen == Screen::Viewing) {
    g.picture = false;
  }
  animate();
}

void tick() {
  if (g.screen == Screen::Hosting && g.host) {
    HostPipeline::Info i = g.host->info();
    std::string s;
    if (!i.error.empty()) {
      s = "A kép nem indult el: " + i.error;
    } else if (i.width) {
      char b[160];
      std::snprintf(b, sizeof b, "Kép: %d×%d · %s%s", i.width, i.height, i.encoder.c_str(), i.hardware ? " (videókártya)" : "");
      s = b;
      if (i.fps > 0) {
        std::snprintf(b, sizeof b, " · %.0f FPS · %.1f ms/kép", i.fps, i.encodeMs);
        s += b;
      }
    } else {
      s = "A kép indul…";
    }
    s += i.audioLayout.empty() ? " · hang nélkül" : " · hang: " + i.audioLayout;
    g.info = toW(s);
    invalidate();
  } else if (g.screen == Screen::Viewing && g.player) {
    VideoPlayer::Stats st = g.player->stats();
    if (g.engine && st.latencyMs >= 0) g.engine->reportLatency(st.latencyMs, st.fps);
    const Json& v = g.viewStats;
    std::string s;
    if (g.picture && st.width) {
      char b[200];
      std::snprintf(b, sizeof b, "%d×%d · %.0f FPS", st.width, st.height, st.fps);
      s = b;
      if (st.latencyMs >= 0) s += " · késés " + std::to_string(int(std::lround(st.latencyMs))) + " ms";
      if (v["path"].isString()) s += " · " + narrow(pathName(v["path"].str()).c_str());
      if (g.stats) {
        if (v["kbps"].isNumber()) {
          std::snprintf(b, sizeof b, " · %.1f Mbit/s", v["kbps"].num() / 1000);
          s += b;
        }
        if (v["rtt"].isNumber()) s += " · RTT " + std::to_string(int(std::lround(v["rtt"].num()))) + " ms";
        if (v["loss"].isNumber()) {
          std::snprintf(b, sizeof b, " · veszteség %.1f%%", v["loss"].num());
          s += b;
        }
        s += std::string(" · dekódolás: ") + (st.hardware ? "videókártya" : "processzor");
        if (v["encoder"].isString()) s += " · küldő: " + v["encoder"].str();
      } else {
        s += " · S: részletek";
      }
    } else {
      s = "Várakozás a képre…";
    }
    g.viewInfo = toW(s);
    invalidate();
  }
}

/* ------------------------------------------------ full screen */

void toggleFullscreen() {
  if (g.screen != Screen::Viewing) return;
  if (!g.fullscreen) {
    GetWindowPlacement(g.wnd, &g.placement);
    g.savedStyle = GetWindowLongPtrW(g.wnd, GWL_STYLE);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongPtrW(g.wnd, GWL_STYLE, (g.savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    g.fullscreen = true;
    SetWindowPos(g.wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else {
    g.fullscreen = false;
    SetWindowLongPtrW(g.wnd, GWL_STYLE, g.savedStyle);
    SetWindowPlacement(g.wnd, &g.placement);
    SetWindowPos(g.wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  updateChildren();
  invalidate();
}

void setMuted(bool muted) {
  g.muted = muted;
  if (g.audioOut) g.audioOut->setVolume(muted ? 0.0f : g.volume);
  animate();
}

void setVolumeAt(float x) {
  RECT rc;
  GetClientRect(g.wnd, &rc);
  Rect bar = barLayout(dip(rc.right)).volume;
  g.volume = std::clamp((x - bar.x) / bar.w, 0.0f, 1.0f);
  saveDword(L"Volume", DWORD(std::lround(g.volume * 100)));
  setMuted(false);
}

/* ------------------------------------------------ updates */

void checkUpdatesLater() {
  HWND wnd = g.wnd;
  std::thread([wnd] {
    Sleep(5000);
    for (;;) {
      UpdateInfo u;
      if (checkForUpdate(SS_VERSION, u)) {
        PostMessageW(wnd, WM_UPDATE, 0, LPARAM(new UpdateInfo(u)));
        return;
      }
      Sleep(3 * 3600 * 1000);
    }
  }).detach();
}

void checkUpdatesNow() {
  if (g.checking) return;
  g.checking = true;
  g.updateNote.clear();
  refreshAbout();
  HWND wnd = g.wnd;
  std::thread([wnd] {
    UpdateInfo u;
    bool found = checkForUpdate(SS_VERSION, u);
    PostMessageW(wnd, WM_UPDATE, 1, LPARAM(found ? new UpdateInfo(u) : nullptr));
  }).detach();
  animate();
}

void installUpdate() {
  if (g.updating || g.update.version.empty()) return;
  g.updating = true;
  g.updateProgress = 0;
  HWND wnd = g.wnd;
  UpdateInfo u = g.update;
  std::thread([wnd, u] {
    bool ok = downloadAndInstall(u, [wnd](int pct) { PostMessageW(wnd, WM_UPDATE_PROGRESS, WPARAM(pct), 0); });
    PostMessageW(wnd, WM_UPDATE_DONE, ok, 0);
  }).detach();
  refreshAbout();
  animate();
}

/* ------------------------------------------------ the title bar, the About window */

/** The title bar in the app's colours (Windows 10 2004+ dark mode; Windows 11 caption colour, round corners). */
void styleTitleBar(HWND wnd) {
  typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
  static SetAttr set = [] {
    SetAttr f = nullptr;
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
      FARPROC p = GetProcAddress(dwm, "DwmSetWindowAttribute");
      std::memcpy(&f, &p, sizeof f);
    }
    return f;
  }();
  if (!set) return;
  BOOL dark = TRUE;
  set(wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
  COLORREF caption = kBgGdi, text = kTextGdi;
  set(wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
  set(wnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof text);
  DWORD round = 2; // DWMWCP_ROUND
  set(wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
}

const float kAboutW = 320, kAboutH = 384;

void showAbout() {
  if (g.about) {
    SetForegroundWindow(g.about);
    return;
  }
  UINT dpi = GetDpiForWindow(g.wnd);
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
  RECT r{0, 0, MulDiv(int(kAboutW), int(dpi), 96), MulDiv(int(kAboutH), int(dpi), 96)};
  AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
  int w = r.right - r.left, h = r.bottom - r.top;
  RECT owner;
  GetWindowRect(g.wnd, &owner);
  int x = (owner.left + owner.right) / 2 - w / 2, y = owner.top + (owner.bottom - owner.top) * 2 / 5 - h / 2;
  g.aboutAnim = Anims();
  g.aboutHover = g.aboutPressed = ID_NONE;
  g.about = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SSAbout", L"A Screen Share névjegye", style, x, std::max<int>(int(owner.top), y), w, h, g.wnd, nullptr, g.inst, nullptr);
  if (!g.about) return;
  styleTitleBar(g.about);
  ShowWindow(g.about, SW_SHOW);
}

void paintAbout() {
  Canvas& c = g.aboutCanvas;
  if (!c.begin(g.about, float(GetDpiForWindow(g.about)))) return;
  // the widgets draw into this window: its canvas, its animations, its buttons, its hover
  int hover = g.hover, pressed = g.pressed;
  g.hover = g.aboutHover;
  g.pressed = g.aboutPressed;
  g.paintingAbout = true;
  g.aboutAnim.frame(nowMs());
  g.aboutHits.clear();
  drawAboutWindow(c.width(), c.height());
  c.end();
  g.paintingAbout = false;
  g.hover = hover;
  g.pressed = pressed;
  if (g.aboutAnim.active() != g.aboutTimer) {
    g.aboutTimer = g.aboutAnim.active();
    if (g.aboutTimer) SetTimer(g.about, TIMER_ANIM, 15, nullptr);
    else KillTimer(g.about, TIMER_ANIM);
  }
}

int aboutHitAt(LPARAM lp) {
  float k = 96.0f / float(GetDpiForWindow(g.about));
  float x = float(GET_X_LPARAM(lp)) * k, y = float(GET_Y_LPARAM(lp)) * k;
  for (auto it = g.aboutHits.rbegin(); it != g.aboutHits.rend(); ++it)
    if (it->r.contains(x, y)) return it->id;
  return ID_NONE;
}

LRESULT CALLBACK aboutProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      paintAbout();
      EndPaint(h, &ps);
      return 0;
    }
    case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE: {
      if (!g.aboutTracking) {
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        g.aboutTracking = true;
      }
      int id = aboutHitAt(lp);
      if (id != g.aboutHover) {
        g.aboutHover = id;
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g.aboutTracking = false;
      g.aboutHover = ID_NONE;
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, g.aboutHover != ID_NONE ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      return DefWindowProcW(h, msg, wp, lp);
    case WM_LBUTTONDOWN:
      g.aboutPressed = aboutHitAt(lp);
      SetCapture(h);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_LBUTTONUP: {
      ReleaseCapture();
      int id = aboutHitAt(lp), was = g.aboutPressed;
      g.aboutPressed = ID_NONE;
      if (id != ID_NONE && id == was) {
        if (id == B_CHECK) checkUpdatesNow();
        else if (id == B_UPDATE) installUpdate();
        else if (id == B_SITE) ShellExecuteW(h, L"open", kSite, nullptr, nullptr, SW_SHOWNORMAL);
      }
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE || wp == VK_RETURN) DestroyWindow(h);
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_DESTROY:
      g.about = nullptr;
      g.aboutTimer = g.aboutTracking = false;
      g.aboutHits.clear();
      return 0;
    default:
      return DefWindowProcW(h, msg, wp, lp);
  }
}

/* ------------------------------------------------ input */

void copyLink() {
  if (g.roomLink.empty() || !OpenClipboard(g.wnd)) return;
  std::wstring t = toW(g.roomLink);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
  std::memcpy(GlobalLock(mem), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(mem);
  EmptyClipboard();
  SetClipboardData(CF_UNICODETEXT, mem);
  CloseClipboard();
  g.copiedAt = nowMs();
  animate();
}

void onClick(int id) {
  if (id >= B_MONITOR0 && id < B_MONITOR0 + 100) g.monitor = id - B_MONITOR0;
  else if (id >= B_QUALITY0 && id < B_QUALITY0 + 100) g.quality = id - B_QUALITY0;
  switch (id) {
    case B_SHARE: startSharing(); break;
    case B_JOIN: startWatching(parseCode(getText(g.code))); break;
    case B_REMEMBER: g.remember = !g.remember; break;
    case B_AUDIO: g.audio = !g.audio; break;
    case B_ABOUT: showAbout(); break;
    case B_COPY: copyLink(); break;
    case B_PAUSE:
      g.live = !g.live;
      if (g.engine) g.engine->setLive(g.live);
      break;
    case B_STOP:
      ask(L"Befejezed a megosztást?", L"A nézőknél megáll a kép, és a szoba bezárul.", L"Befejezés", L"Mégse", true, [] { stopToHome(); });
      break;
    case B_MODAL_OK: closeModal(true); break;
    case B_MODAL_CANCEL: closeModal(false); break;
    case B_FULL: toggleFullscreen(); break;
    case B_LEAVE: stopToHome(); break;
    case B_MUTE: setMuted(!g.muted); break;
    case B_UPDATE: installUpdate(); break;
    case B_CHECK: checkUpdatesNow(); break;
    case B_SITE: ShellExecuteW(g.wnd, L"open", kSite, nullptr, nullptr, SW_SHOWNORMAL); break;
    default: break;
  }
  animate();
}

int hitAt(float x, float y) {
  for (auto it = g.hits.rbegin(); it != g.hits.rend(); ++it)
    if (it->r.contains(x, y)) return it->id;
  return ID_NONE;
}

/* ------------------------------------------------ window procedures */

LRESULT CALLBACK videoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_LBUTTONDBLCLK: toggleFullscreen(); return 0;
    case WM_LBUTTONDOWN: SetFocus(g.wnd); return 0;
    case WM_ERASEBKGND: return 1; // the swap chain draws
    default: return DefWindowProcW(h, msg, wp, lp);
  }
}

/** The text boxes: Enter starts what the box is for; the focus ring follows them. */
LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
  if (msg == WM_KEYDOWN && wp == VK_RETURN) {
    if (int(id) == ID_PASSWORD) startSharing();
    else if (int(id) == ID_CODE) startWatching(parseCode(getText(g.code)));
    return 0;
  }
  if (msg == WM_CHAR && (wp == VK_RETURN || wp == 10)) return 0; // no beep
  if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
    if (msg == WM_SETFOCUS) g.focus = int(id);
    else if (g.focus == int(id)) g.focus = ID_NONE;
    if (int(id) == ID_CODE && msg == WM_SETFOCUS && !g.joinErr.empty()) g.joinErr.clear();
    animate();
  }
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, editProc, id);
  return DefSubclassProc(h, msg, wp, lp);
}

HWND makeEdit(int id, DWORD style, const std::wstring& text, const wchar_t* cue) {
  HWND h = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | style, 0, 0, 10, 10, g.wnd, reinterpret_cast<HMENU>(intptr_t(id)), g.inst,
                           nullptr);
  SendMessageW(h, WM_SETFONT, WPARAM(g.editFont), TRUE);
  SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
  if (cue) SendMessageW(h, EM_SETCUEBANNER, TRUE, LPARAM(cue));
  SetWindowSubclass(h, editProc, UINT_PTR(id), 0);
  return h;
}

void makeEditFont() {
  HFONT old = g.editFont;
  g.editFont = CreateFontW(-px(14.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                           L"Segoe UI");
  for (HWND h : {g.pass, g.code})
    if (h) SendMessageW(h, WM_SETFONT, WPARAM(g.editFont), TRUE);
  if (old) DeleteObject(old);
  g.placed.clear();
}

void createControls() {
  makeEditFont();
  g.fieldBrush = CreateSolidBrush(kFieldGdi);
  std::wstring pw = loadPassword();
  g.pass = makeEdit(ID_PASSWORD, ES_PASSWORD, pw, L"jelszó");
  g.code = makeEdit(ID_CODE, 0, L"", L"pl. abc123 vagy https://mozizas.hu/#abc123");
  g.video = CreateWindowExW(0, L"SSVideo", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, g.wnd, nullptr, g.inst, nullptr);
  g.remember = !pw.empty();
  g.audio = loadDword(L"Audio", 1) != 0;
  g.quality = int(std::min<DWORD>(loadDword(L"Quality", 0), DWORD(std::size(kQualities) - 1)));
  g.monitors = listMonitors();
  g.monitor = int(std::min<DWORD>(loadDword(L"Monitor", 0), DWORD(g.monitors.empty() ? 0 : g.monitors.size() - 1)));
  g.volume = float(std::min<DWORD>(loadDword(L"Volume", 100), 100)) / 100.0f;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g.wnd = h;
      g.dpi = GetDpiForWindow(h);
      createControls();
      if (HMENU sys = GetSystemMenu(h, FALSE)) {
        AppendMenuW(sys, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sys, MF_STRING, kSysAbout, L"A Screen Share névjegye");
      }
      SetTimer(h, TIMER_TICK, 1000, nullptr);
      checkUpdatesLater();
      return 0;
    case WM_DPICHANGED: {
      g.dpi = HIWORD(wp);
      makeEditFont();
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      invalidate();
      return 0;
    }
    case WM_SIZE:
      g.placed.erase(-1);
      invalidate();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {px(760), px(560)};
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      paint();
      EndPaint(h, &ps);
      return 0;
    }
    case WM_CTLCOLOREDIT:
      SetBkColor(HDC(wp), kFieldGdi);
      SetTextColor(HDC(wp), kTextGdi);
      return LRESULT(g.fieldBrush);
    case WM_MOUSEMOVE: {
      float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
      if (!g.tracking) {
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        g.tracking = true;
      }
      if (g.draggingVolume) setVolumeAt(x);
      int id = hitAt(x, y);
      if (id != g.hover) {
        g.hover = id;
        animate();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g.tracking = false;
      if (g.hover != ID_NONE) {
        g.hover = ID_NONE;
        animate();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT && HWND(wp) == h) {
        SetCursor(LoadCursorW(nullptr, g.hover != ID_NONE ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      return DefWindowProcW(h, msg, wp, lp);
    case WM_LBUTTONDOWN: {
      float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
      g.pressed = hitAt(x, y);
      if (g.pressed == ID_NONE) SetFocus(h); // a click beside the text boxes takes the focus from them
      SetCapture(h);
      if (g.pressed == B_VOLUME) {
        g.draggingVolume = true;
        setVolumeAt(x);
      }
      animate();
      return 0;
    }
    case WM_LBUTTONUP: {
      ReleaseCapture();
      float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
      int id = hitAt(x, y), was = g.pressed;
      g.pressed = ID_NONE;
      g.draggingVolume = false;
      if (id != ID_NONE && id == was && id != B_VOLUME) onClick(id);
      animate();
      return 0;
    }
    case WM_MOUSEWHEEL:
      g.scrollTarget = std::max(0.0f, g.scrollTarget - float(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f * 64.0f);
      animate();
      return 0;
    case WM_KEYDOWN:
      if (g.modal.open) {
        if (wp == VK_ESCAPE) closeModal(false);
        else if (wp == VK_RETURN) closeModal(true);
        return 0;
      }
      if (g.screen == Screen::Viewing) {
        if (wp == 'F' || (wp == VK_ESCAPE && g.fullscreen)) toggleFullscreen();
        else if (wp == 'S') {
          g.stats = !g.stats;
          tick();
        } else if (wp == 'M') setMuted(!g.muted);
      }
      return 0;
    case WM_TIMER:
      if (wp == TIMER_TICK) tick();
      if (wp == TIMER_ANIM) invalidate();
      return 0;
    case WM_ENGINE: {
      std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
      if (int(wp) == g.session) onEngineEvent(Json::parse(*text));
      return 0;
    }
    case WM_UPDATE: {
      std::unique_ptr<UpdateInfo> u(reinterpret_cast<UpdateInfo*>(lp));
      if (wp == 1) g.checking = false;
      if (u) {
        g.update = *u;
        g.updateNote = L"Új verzió érhető el.";
      } else {
        g.updateNote = L"Ez a legújabb verzió.";
      }
      refreshAbout();
      animate();
      return 0;
    }
    case WM_UPDATE_PROGRESS:
      g.updateProgress = int(wp);
      refreshAbout();
      animate();
      return 0;
    case WM_UPDATE_DONE:
      if (wp) {
        DestroyWindow(h); // the installer starts the new version
      } else {
        g.updating = false;
        g.updateProgress = -1;
        g.updateNote = L"A frissítés nem sikerült – próbáld újra.";
        refreshAbout();
        animate();
      }
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == kSysAbout) {
        showAbout();
        return 0;
      }
      return DefWindowProcW(h, msg, wp, lp);
    case WM_CLOSE:
      if (g.screen == Screen::Hosting) {
        ask(L"Kilépsz?", L"A megosztás befejeződik: a nézőknél megáll a kép, és a szoba bezárul.", L"Kilépés", L"Mégse", true, [h] { DestroyWindow(h); });
        return 0;
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      endSession();
      Sleep(150); // the goodbye to the server leaves before the process ends
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(h, msg, wp, lp);
  }
}

/* ------------------------------------------------ smoke tests (CI): the real pipeline, no UI */

// --smoke-host <server> <password> <seconds> <log>: captures, encodes and shares the main screen
// --smoke-view <server> <code> <seconds> <log>: watches a room and decodes what arrives
int smoke(int argc, wchar_t** argv) {
  std::string mode = narrow(argv[1]), server = narrow(argv[2]), secret = narrow(argv[3]);
  int seconds = _wtoi(argv[4]);
  FILE* log = _wfopen(argv[5], L"w");
  auto line = [log](const std::string& s) {
    std::fprintf(log, "%s\n", s.c_str());
    std::fflush(log);
  };
  EngineConfig cfg;
  cfg.server = server;
  cfg.webSocket = openWinHttpWebSocket;
  cfg.forceTcp = GetEnvironmentVariableA("SS_FORCE_TCP", nullptr, 0) > 0; // (CI: the WebSocket path, as to a browser)
  EngineCallbacks cb;
  cb.event = [&](const Json& e) {
    if (e["type"].str() == "hosted") line("CODE " + e["code"].str());
    if (e["type"].str() != "viewers") line("EVENT " + e.dump());
  };
  Json result = Json::object();
  if (mode == "--smoke-host") {
    HostPipeline* hostPtr = nullptr;
    cb.streaming = [&](bool on) { if (hostPtr) hostPtr->setStreaming(on); };
    cb.keyframe = [&] { if (hostPtr) hostPtr->keyframe(); };
    cb.bitrate = [&](double bps) { if (hostPtr) hostPtr->setBitrate(bps); };
    Engine engine(cfg, cb);
    HostPipeline host(engine);
    hostPtr = &host;
    std::vector<Monitor> mons = listMonitors();
    HostPipeline::Options o;
    if (!mons.empty()) o.monitor = mons[0].rect;
    o.audio = true;
    o.fps = 30;
    host.start(o);
    engine.host(secret);
    Sleep(DWORD(seconds) * 1000);
    HostPipeline::Info i = host.info();
    result.set("encoder", i.encoder).set("hardware", i.hardware).set("width", i.width).set("height", i.height).set("fps", i.fps).set("encodeMs", i.encodeMs);
    result.set("error", i.error).set("audio", i.audioLayout).set("audioError", i.audioError).set("monitors", int(mons.size()));
    host.stop();
    hostPtr = nullptr;
  } else {
    HWND wnd = CreateWindowExW(0, L"SSVideo", L"smoke", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    VideoPlayer player(wnd);
    AudioPlayer audio;
    std::atomic<int> frames{0}, sounds{0};
    cb.media = [&](const MediaMeta& m, const uint8_t* d, size_t n) {
      if (m.stream == 0) {
        frames++;
        player.push(d, n, m.key, m.capture);
      } else {
        sounds++;
        audio.push(m.sub, d, n, m.ts);
      }
    };
    Engine engine(cfg, cb);
    player.clockOffset = [&] { return engine.clockOffset(); };
    player.needKeyframe = [&] { engine.requestKeyframe(); };
    engine.join(secret);
    double best = 0;
    VideoPlayer::Stats st;
    for (int t = 0; t < seconds * 10; t++) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
      Sleep(100);
      st = player.stats();
      best = std::max(best, st.fps);
    }
    result.set("received", frames.load()).set("audioPackets", sounds.load()).set("shownFps", best).set("width", st.width).set("height", st.height);
    result.set("hardwareDecode", st.hardware).set("latencyMs", st.latencyMs);
    DestroyWindow(wnd);
  }
  line("RESULT " + result.dump());
  std::fclose(log);
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int showCmd) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  MFStartup(MF_VERSION, MFSTARTUP_LITE);
  netInit();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  timeBeginPeriod(1); // 1 ms timers: the capture, the network loop and the animations wake on time
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  g.inst = inst;
  HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hIcon = icon;
  wc.hIconSm = icon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(kBgGdi);
  wc.lpszClassName = L"ScreenShare";
  RegisterClassExW(&wc);
  WNDCLASSEXW vc{sizeof vc};
  vc.style = CS_DBLCLKS;
  vc.lpfnWndProc = videoProc;
  vc.hInstance = inst;
  vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  vc.lpszClassName = L"SSVideo";
  RegisterClassExW(&vc);
  WNDCLASSEXW ac{sizeof ac};
  ac.lpfnWndProc = aboutProc;
  ac.hInstance = inst;
  ac.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  ac.hbrBackground = CreateSolidBrush(kBgGdi);
  ac.lpszClassName = L"SSAbout";
  RegisterClassExW(&ac);
  if (argc == 6 && (std::wstring(argv[1]) == L"--smoke-host" || std::wstring(argv[1]) == L"--smoke-view")) return smoke(argc, argv);
  HWND wnd = CreateWindowExW(0, L"ScreenShare", L"Screen Share", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1120, 800, nullptr, nullptr, inst,
                             nullptr);
  styleTitleBar(wnd);
  // a room link given on the command line (screenshare://abc123 or the code)
  std::wstring arg = cmd ? cmd : L"";
  ShowWindow(wnd, showCmd);
  UpdateWindow(wnd);
  if (!arg.empty() && arg.find(L"/") != 0) {
    std::string code = parseCode(arg);
    if (code.size() >= 4) startWatching(code);
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (msg.message == WM_KEYDOWN && msg.hwnd != g.about && (g.modal.open || g.screen == Screen::Viewing)) {
      // keys in the viewer go to the window, whichever child has the focus
      SendMessageW(g.wnd, WM_KEYDOWN, msg.wParam, msg.lParam);
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && (msg.hwnd == g.pass || msg.hwnd == g.code)) {
      if (msg.hwnd == g.pass) startSharing();
      else startWatching(parseCode(getText(g.code)));
      continue;
    }
    if (IsDialogMessageW(wnd, &msg)) continue; // Tab between the text boxes
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  timeEndPeriod(1);
  MFShutdown();
  CoUninitialize();
  return 0;
}
