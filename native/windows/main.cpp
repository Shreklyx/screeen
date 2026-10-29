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
#include <dwmapi.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#include "ws_bench.h"

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
const UINT WM_CLOSE_FOR_UPDATE = WM_APP + 6; // from another window or the installer: end, an update replaces the app

const UINT_PTR TIMER_TICK = 1, TIMER_ANIM = 3;

const char* kServer = "https://mozizas.hu";
const wchar_t* kSite = L"https://mozizas.hu";
const wchar_t* kVersion = WIDEN(SS_VERSION);
const wchar_t* kBuild = WIDEN(SS_BUILD);
const wchar_t* kCopyright = L"Copyright © 2026 mozizas.hu.\nMinden jog fenntartva.";
const UINT kSysAbout = 0x1010, kSysNew = 0x1020; // "About" and "New window" in the window menu (Alt+Space)

// the palette: the Mac app's (macOS in its dark appearance), with the app's pink
const Color cWin1 = rgb(0x323237), cWin2 = rgb(0x29292E), cText = rgb(0xEDEDF0), cMuted = rgb(0xA3A3AB), cFaint = rgb(0x77777F),
            cAccent = rgb(0xF74F9F), cField = rgb(0x1E1E21), cOk = rgb(0x32D74B), cWarn = rgb(0xFF9F0A), cErr = rgb(0xFF6961),
            cWhite = rgb(0xFFFFFF);
const COLORREF kFieldGdi = RGB(0x1E, 0x1E, 0x21), kTextGdi = RGB(0xED, 0xED, 0xF0), kBgGdi = RGB(0x32, 0x32, 0x37);

// the compact screens (home, sharing): their content's width, and the space around it
const float kHomeW = 420, kHostW = 440, kSide = 30, kTopPad = 24, kBottomPad = 26;

enum class Screen { Home, Hosting, Viewing };

// what can be clicked (and the text boxes)
enum Id {
  ID_NONE = 0,
  ID_PASSWORD = 100, ID_CODE,
  B_SHARE = 200, B_JOIN, B_REMEMBER, B_AUDIO, B_UPDATE, B_ABOUT, B_COPY, B_PAUSE, B_STOP, B_FULL, B_LEAVE, B_VOLUME, B_MUTE,
  B_CHECK, B_SITE, B_MODAL_OK, B_MODAL_CANCEL, B_TAB0, B_TAB1, B_MONITOR, B_QUALITY, B_NEW,
};
const UINT kMenuMonitor0 = 1000, kMenuQuality0 = 2000; // the pop-up menus' items

struct Quality {
  const wchar_t* name;  // in the pop-up
  const wchar_t* hint;  // under the settings
  int w, h, fps;
};
const Quality kQualities[] = {
    {L"Automatikus", L"A legszebb, amit a gép és a hálózat bír: akár 4K, 60 FPS.", 3840, 2160, 60},
    {L"1440p · 60 FPS", L"2560×1440, 60 FPS.", 2560, 1440, 60},
    {L"1080p · 60 FPS", L"1920×1080, 60 FPS.", 1920, 1080, 60},
    {L"1080p · 120 FPS", L"1920×1080, 120 FPS – játékhoz, gyors mozgáshoz.", 1920, 1080, 120},
    {L"720p · 60 FPS", L"1280×720, 60 FPS – gyenge internetre.", 1280, 720, 60},
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
  int tab = 0; // home: 0 sharing, 1 watching

  HWND pass = nullptr, code = nullptr, video = nullptr;
  std::map<int, Rect> fieldRects;   // where the text boxes go (DIPs, in the scrolled content)
  std::map<int, Rect> controlRects; // where the pop-ups are (their menus open under them)
  std::map<int, RECT> placed;       // where the text boxes are (pixels)
  int focus = ID_NONE;

  Canvas canvas;
  Anims anim;
  bool animTimer = false;
  std::vector<Hit> hits;
  std::map<int, std::wstring> tips; // what an icon-only button does (shown when the mouse rests on it)
  int hover = ID_NONE, pressed = ID_NONE;
  double hoverSince = 0;
  bool tracking = false, measuring = false;
  float scroll = 0, scrollTarget = 0, contentHeight = 0;
  bool fitPosted = false;
  POINT anchor{}; // the compact window's top centre, where it goes back to after watching
  bool anchored = false;

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

  bool shots = false;            // --ui-shot, --ui-watch (CI): no update check
  std::string server = kServer;  // (CI: a local server)
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

/** What a button without a label does: shown under it once the mouse rests there. */
void tip(int id, const std::wstring& text) {
  if (!g.measuring && !g.paintingAbout && !g.transitioning) g.tips[id] = text;
}

void drawTip(float W, float H) {
  auto it = g.tips.find(g.hover);
  if (it == g.tips.end() || g.modal.open || g.pressed != ID_NONE) return;
  const Rect* at = nullptr;
  for (const Hit& h : g.hits)
    if (h.id == g.hover) at = &h.r;
  if (!at) return;
  double held = A().now() - g.hoverSince;
  if (held < 600) {
    A().keepGoing();
    return;
  }
  float a = std::clamp(float((held - 600) / 120), 0.0f, 1.0f);
  if (a < 1) A().keepGoing();
  Canvas& c = C();
  float tw = c.measure(it->second, 12, Regular), w = tw + 18, h = 24;
  float x = std::clamp(at->cx() - w / 2, 6.0f, std::max(6.0f, W - w - 6)), y = at->bottom() + 6;
  if (y + h > H - 6) y = at->y - h - 6;
  c.pushAlpha(a);
  c.fillRound({x, y, w, h}, 6, rgb(0x18181C, 0.97f));
  c.strokeRound({x, y, w, h}, 6, rgb(0xFFFFFF, 0.12f), 1);
  c.text(it->second, {x, y, w, h}, 12, Regular, cText, Center);
  c.popAlpha();
}

/** A settings group, as in the Mac app (and System Settings): a faint panel, its rows split by lines. */
void group(const Rect& r, std::initializer_list<float> lines = {}) {
  if (g.measuring) return;
  Canvas& c = C();
  c.fillRound(r, 11, rgb(0xFFFFFF, 0.055f));
  c.strokeRound(r, 11, rgb(0xFFFFFF, 0.085f), 1);
  for (float y : lines) c.fillRect({r.x + 12, std::floor(y), r.w - 24, 1}, rgb(0xFFFFFF, 0.09f));
}

/** A row's title on the left (with a smaller second line under it). */
void rowTitle(const Rect& row, const std::wstring& title, const std::wstring& sub = L"") {
  if (g.measuring) return;
  Canvas& c = C();
  if (sub.empty()) {
    c.text(title, {row.x + 12, row.y, row.w * 0.5f, row.h}, 13, Regular, cText);
    return;
  }
  c.text(title, {row.x + 12, row.cy() - 17, row.w * 0.5f, 18}, 13, Regular, cText);
  c.text(sub, {row.x + 12, row.cy() + 1, row.w * 0.5f, 15}, 11, Regular, cMuted);
}

enum class Style { Primary, Secondary, Danger, Ghost, Success };

/** A button: a capsule, as on macOS 26 (the pink one is the screen's main action). */
void button(int id, const Rect& r, const std::wstring& label, Style style, bool hasIcon = false, Icon icon = Icon::Play, bool enabled = true) {
  if (g.measuring) return;
  if (enabled) hit(id, r);
  float hov = A().to(key("h", id), enabled && g.hover == id ? 1.0f : 0.0f, 120);
  float prs = A().to(key("p", id), enabled && g.pressed == id && g.hover == id ? 1.0f : 0.0f, 70);
  Canvas& c = C();
  float radius = r.h / 2;
  Color text = cText;
  switch (style) {
    case Style::Primary:
      c.fillRound(r, radius, mix(mix(cAccent, cWhite, 0.10f * hov), rgb(0x000000), 0.14f * prs));
      text = cWhite;
      break;
    case Style::Secondary:
      c.fillRound(r, radius, rgb(0xFFFFFF, 0.12f + 0.05f * hov + 0.05f * prs));
      break;
    case Style::Danger:
      c.fillRound(r, radius, rgb(0xFFFFFF, 0.12f + 0.05f * hov + 0.05f * prs));
      text = cErr;
      break;
    case Style::Success:
      c.fillRound(r, radius, withAlpha(cOk, 0.16f + 0.05f * hov));
      text = cOk;
      break;
    case Style::Ghost:
      c.fillRound(r, radius, rgb(0xFFFFFF, 0.08f * hov + 0.05f * prs));
      text = mix(cMuted, cText, hov);
      break;
  }
  if (!enabled) text = withAlpha(text, 0.5f);
  float tw = label.empty() ? 0 : c.measure(label, 13, Semibold);
  float iw = hasIcon ? 15 : 0, gap = hasIcon && !label.empty() ? 7 : 0;
  float x = r.cx() - (tw + iw + gap) / 2;
  if (hasIcon) c.icon(icon, x + iw / 2, r.cy(), 15, text, 1.7f);
  if (!label.empty()) c.text(label, {x + iw + gap, r.y, tw + 4, r.h}, 13, Semibold, text);
}

/** An on/off switch (the Mac's) at the right of its row; the whole row toggles it. */
void toggleSwitch(int id, const Rect& row, bool on) {
  if (g.measuring) return;
  hit(id, row);
  Canvas& c = C();
  float k = A().to(key("t", id), on ? 1.0f : 0.0f, 170);
  float hov = A().to(key("h", id), g.hover == id ? 1.0f : 0.0f, 120);
  Rect track{row.right() - 12 - 42, row.cy() - 11, 42, 22};
  c.fillRound(track, 11, mix(rgb(0xFFFFFF, 0.16f + 0.05f * hov), cAccent, k));
  float kw = 25, kx = track.x + 2 + k * (track.w - 4 - kw);
  Rect knob{kx, track.y + 2, kw, track.h - 4};
  c.fillRound(knob.offset(0, 0.7f), knob.h / 2, rgb(0x000000, 0.25f));
  c.fillRound(knob, knob.h / 2, cWhite);
}

/** A text box: drawn here, the Windows edit control sits inside it. */
void field(int id, const Rect& r) {
  g.fieldRects[id] = r;
  if (g.measuring) return;
  Canvas& c = C();
  float f = A().to(key("f", id), g.focus == id ? 1.0f : 0.0f, 150);
  if (f > 0.01f) c.strokeRound({r.x - 3, r.y - 3, r.w + 6, r.h + 6}, 9, withAlpha(cAccent, 0.6f * f), 3); // the focus ring
  c.fillRound(r, 6, cField);
  c.strokeRound(r, 6, rgb(0xFFFFFF, 0.14f), 1);
  // while the screens cross-fade the edit control is hidden: its text is drawn instead
  HWND h = id == ID_PASSWORD ? g.pass : g.code;
  if (g.transitioning && h) {
    std::wstring t = getText(h);
    if (id == ID_PASSWORD) t = std::wstring(t.size(), L'\x25CF');
    c.text(t, {r.x + 8, r.y, r.w - 16, r.h}, 13, Regular, cText);
  }
}

/** How wide a pop-up is for these choices (as wide as the longest, as the Mac's). */
template <typename Fn>
float popupWidth(size_t n, Fn name, float most) {
  float w = 60;
  for (size_t i = 0; i < n; i++) w = std::max(w, C().measure(name(i), 13, Regular));
  return std::min(most, std::ceil(w) + 10 + 30);
}

/** A pop-up button (the Mac's): the choice and the up-down arrows; a click opens its menu. */
void popup(int id, const Rect& r, const std::wstring& label, bool enabled = true) {
  g.controlRects[id] = r;
  if (g.measuring) return;
  if (enabled) hit(id, r);
  Canvas& c = C();
  float hov = A().to(key("h", id), enabled && g.hover == id ? 1.0f : 0.0f, 120);
  float prs = A().to(key("p", id), enabled && g.pressed == id ? 1.0f : 0.0f, 70);
  c.fillRound(r, 6, rgb(0xFFFFFF, 0.165f + 0.05f * hov + 0.06f * prs));
  c.strokeRound(r, 6, rgb(0xFFFFFF, 0.05f), 1);
  Color tc = enabled ? cText : cFaint;
  c.text(label, {r.x + 10, r.y, r.w - 34, r.h}, 13, Regular, tc);
  float ax = r.right() - 14, ay = r.cy();
  c.line(ax - 3, ay - 2, ax, ay - 5, tc, 1.4f);
  c.line(ax, ay - 5, ax + 3, ay - 2, tc, 1.4f);
  c.line(ax - 3, ay + 2, ax, ay + 5, tc, 1.4f);
  c.line(ax, ay + 5, ax + 3, ay + 2, tc, 1.4f);
}

/** Megosztás | Nézés (the Mac's segmented control): the pink one slides to the chosen side. */
void segmented(const Rect& r, int selected) {
  if (g.measuring) return;
  Canvas& c = C();
  const wchar_t* labels[2] = {L"Megosztás", L"Nézés"};
  float sw = r.w / 2;
  float pos = A().to("tabpos", float(selected), 190);
  c.fillRound(r, r.h / 2, rgb(0xFFFFFF, 0.12f));
  Rect pill{r.x + 2 + pos * sw, r.y + 2, sw - 4, r.h - 4};
  c.fillRound(pill, pill.h / 2, cAccent);
  for (int i = 0; i < 2; i++) {
    Rect seg{r.x + float(i) * sw, r.y, sw, r.h};
    int id = B_TAB0 + i;
    if (i != selected) hit(id, seg);
    float hov = A().to(key("h", id), g.hover == id ? 1.0f : 0.0f, 120);
    if (hov > 0.01f) c.fillRound(seg.inset(2, 2), (r.h - 4) / 2, rgb(0xFFFFFF, 0.07f * hov));
    float on = std::clamp(1 - std::fabs(pos - float(i)), 0.0f, 1.0f);
    c.text(labels[i], seg, 13, Semibold, mix(cText, cWhite, on), Center);
  }
}

void spinner(float cx, float cy, float radius, const Color& col) {
  if (g.measuring) return;
  Canvas& c = C();
  double t = A().now();
  float spin = float(std::fmod(t * 0.40, 360.0));
  float sweep = 70 + 110 * float(0.5 + 0.5 * std::sin(t / 420.0));
  c.strokeCircle(cx, cy, radius, withAlpha(col, 0.18f), 2.2f);
  c.arc(cx, cy, radius, spin, sweep, col, 2.2f);
  A().keepGoing();
}

void dot(float cx, float cy, const Color& col, float r = 4) {
  if (g.measuring) return;
  C().fillCircle(cx, cy, r, col);
}

/** Wrapped text under something (the hints); returns its height. */
float note(const std::wstring& s, float x, float y, float w, float size, const Color& col, Align align = Left) {
  float h = 0;
  C().measure(s, size, Regular, w, &h);
  if (!g.measuring) C().text(s, {x, y, w, h + 2}, size, Regular, col, align, false, true);
  return h;
}

/** An error under a button (fades in); returns where it ends. */
float errorText(const std::wstring& s, const char* animKey, float x, float y, float w) {
  if (s.empty()) return y;
  y += 12;
  float a = g.measuring ? 1.0f : A().to(animKey, 1, 220);
  C().pushAlpha(a);
  y += note(s, x, y, w, 12, cErr, Center);
  C().popAlpha();
  return y;
}

/* ------------------------------------------------ the screens (each returns its content's bottom) */

float sharePane(float x0, float y, float cw) {
  // the password
  group({x0, y, cw, 80}, {y + 40});
  rowTitle({x0, y, cw, 40}, L"Jelszó");
  field(ID_PASSWORD, {x0 + cw - 12 - 210, y + 8, 210, 24});
  rowTitle({x0, y + 40, cw, 40}, L"Jelszó megjegyzése");
  toggleSwitch(B_REMEMBER, {x0, y + 40, cw, 40}, g.remember);
  y += 80 + 14;
  // what, how, with sound
  group({x0, y, cw, 126}, {y + 40, y + 80});
  rowTitle({x0, y, cw, 40}, L"Képernyő");
  float mw = popupWidth(g.monitors.size(), [](size_t i) { return g.monitors[i].name; }, cw * 0.62f);
  popup(B_MONITOR, {x0 + cw - 12 - mw, y + 9, mw, 22}, g.monitors.empty() ? L"Nem találok képernyőt" : g.monitors[size_t(g.monitor)].name, !g.monitors.empty());
  rowTitle({x0, y + 40, cw, 40}, L"Minőség");
  float qw = popupWidth(std::size(kQualities), [](size_t i) { return std::wstring(kQualities[i].name); }, cw * 0.5f);
  popup(B_QUALITY, {x0 + cw - 12 - qw, y + 49, qw, 22}, kQualities[g.quality].name);
  rowTitle({x0, y + 80, cw, 46}, L"A gép hangja", L"5.1 / 7.1 térhang is");
  toggleSwitch(B_AUDIO, {x0, y + 80, cw, 46}, g.audio);
  y += 126 + 6;
  y += note(kQualities[g.quality].hint, x0 + 12, y, cw - 24, 11, cMuted) + 22;
  button(B_SHARE, {std::floor(x0 + cw / 2 - 115), y, 230, 28}, L"Megosztás indítása", Style::Primary);
  y += 28;
  return errorText(g.shareErr, "shareErr", x0, y, cw);
}

float watchPane(float x0, float y, float cw) {
  group({x0, y, cw, 40});
  rowTitle({x0, y, cw, 40}, L"Szobakód vagy link");
  field(ID_CODE, {x0 + cw - 12 - 220, y + 8, 220, 24});
  y += 40 + 6;
  y += note(L"A megosztótól kapott 6 jelű kód, vagy a teljes link (pl. mozizas.hu/#abc123).", x0 + 12, y, cw - 24, 11, cMuted) + 22;
  button(B_JOIN, {std::floor(x0 + cw / 2 - 115), y, 230, 28}, L"Csatlakozás", Style::Primary);
  y += 28;
  return errorText(g.joinErr, "joinErr", x0, y, cw);
}

float drawHome(float W, float) {
  Canvas& c = C();
  float cw = kHomeW, x0 = std::floor((W - cw) / 2), y = kTopPad;
  // a new window (watching more at once) and the About window (the Mac has these in its menus)
  button(B_NEW, {W - 10 - 30 - 4 - 30, 8, 30, 30}, L"", Style::Ghost, true, Icon::Plus);
  tip(B_NEW, L"Új ablak (Ctrl+N)");
  button(B_ABOUT, {W - 10 - 30, 8, 30, 30}, L"", Style::Ghost, true, Icon::Info);
  tip(B_ABOUT, L"Névjegy");
  if (!g.measuring) c.appIcon({std::floor(W / 2 - 42), y, 84, 84});
  y += 84 + 12;
  if (!g.measuring) c.text(L"Screen Share", {x0, y, cw, 28}, 22, Bold, cWhite, Center);
  y += 28 + 4;
  if (!g.measuring) c.text(L"Képernyőmegosztás hanggal, a lehető legkisebb késéssel.", {x0 - kSide, y, cw + 2 * kSide, 18}, 13, Regular, cMuted, Center);
  y += 18 + 24;
  segmented({std::floor(W / 2 - 132), y, 264, 28}, g.tab);
  y += 28 + 22;
  float pane = g.measuring ? 1.0f : A().to("pane", 1, 200); // a new tab fades in
  c.pushAlpha(pane);
  y = g.tab == 0 ? sharePane(x0, y, cw) : watchPane(x0, y, cw);
  c.popAlpha();
  if (!g.update.version.empty()) {
    y += 20;
    Rect r{std::floor(W / 2 - 110), y, 220, 26};
    if (g.updating && g.updateProgress >= 0) {
      if (!g.measuring) {
        float k = A().to("updateBar", float(g.updateProgress) / 100.0f, 250);
        c.fillRound(r, r.h / 2, rgb(0xFFFFFF, 0.12f));
        c.fillRound({r.x, r.y, std::max(r.h, r.w * k), r.h}, r.h / 2, cAccent);
        c.text(L"Letöltés… " + std::to_wstring(g.updateProgress) + L"%", r, 12, Semibold, cWhite, Center);
      }
    } else {
      button(B_UPDATE, r, g.updating ? L"Telepítés…" : L"Frissítés: " + toW(g.update.version), Style::Secondary, true, Icon::Download, !g.updating);
    }
    y += 26;
  }
  return y + kBottomPad;
}

std::wstring pathName(const std::string& path) {
  static const std::map<std::string, std::wstring> names = {
      {"lan", L"helyi hálózat"}, {"internet", L"interneten át"}, {"relay", L"a szerveren át"}, {"turn", L"TURN relén át"}, {"tcp", L"HTTPS-en át"}};
  auto it = names.find(path);
  return it == names.end() ? toW(path) : it->second;
}

Color latencyColor(double ms) { return ms < 40 ? cOk : ms < 90 ? cWarn : cErr; }

/** One viewer: a dot, the name and the way; the numbers on the right. */
void viewerRow(const Json& v, const Rect& r) {
  if (g.measuring) return;
  Canvas& c = C();
  std::string id = v["id"].str(""), state = v["state"].str("");
  float a = A().to("v" + id, 1, 260);
  c.pushAlpha(a);
  bool connected = state == "connected";
  dot(r.x + 16, r.cy(), connected ? cOk : state == "unsupported" ? cErr : state == "retrying" ? cWarn : cFaint);
  // the numbers, right to left; the latency in its colour
  std::vector<std::pair<std::wstring, Color>> parts;
  if (connected) {
    char b[48];
    if (v["fps"].isNumber()) parts.push_back({std::to_wstring(int(v["fps"].num())) + L" FPS", cMuted});
    if (v["kbps"].isNumber()) {
      std::snprintf(b, sizeof b, "%.1f Mbit/s", v["kbps"].num() / 1000);
      parts.push_back({toW(b), cMuted});
    }
    if (v["rtt"].isNumber()) parts.push_back({L"RTT " + std::to_wstring(int(std::lround(v["rtt"].num()))) + L" ms", cMuted});
    if (v["lat"].isNumber()) parts.push_back({L"~" + std::to_wstring(int(std::lround(v["lat"].num()))) + L" ms", latencyColor(v["lat"].num())});
  }
  float x = r.right() - 12;
  for (size_t i = parts.size(); i-- > 0;) {
    bool last = i + 1 == parts.size() && v["lat"].isNumber();
    float w = c.measure(parts[i].first, 11.5f, last ? Semibold : Regular);
    x -= w;
    c.text(parts[i].first, {x, r.y, w + 2, r.h}, 11.5f, last ? Semibold : Regular, parts[i].second);
    if (i > 0) {
      float dw = c.measure(L" · ", 11.5f, Regular);
      x -= dw;
      c.text(L" · ", {x, r.y, dw + 2, r.h}, 11.5f, Regular, cFaint);
    }
  }
  std::wstring sub = connected ? L"közvetlen mód · " + pathName(v["path"].str(""))
                     : state == "unsupported" ? L"nem tud nézni: " + toW(v["whyText"].str(""))
                     : state == "retrying"    ? L"kapcsolat újra… " + toW(v["whyText"].str(""))
                                              : L"kapcsolódik…";
  float tw = std::max(40.0f, x - 12 - (r.x + 30));
  c.text(L"Néző #" + std::to_wstring(int(v["n"].num())), {r.x + 30, r.cy() - 17, tw, 18}, 13, Medium, cText);
  c.text(sub, {r.x + 30, r.cy() + 1, tw, 15}, 11, Regular, cMuted);
  c.popAlpha();
}

float drawHosting(float W, float) {
  Canvas& c = C();
  float cw = kHostW, x0 = std::floor((W - cw) / 2), y = kTopPad;
  // ÉLŐ (or SZÜNETEL), in a little pill
  if (!g.measuring) {
    std::wstring t = g.live ? L"ÉLŐ" : L"SZÜNETEL";
    Color col = g.live ? cErr : cMuted;
    float tw = c.measure(t, 11, Bold);
    Rect pill{std::floor(W / 2 - (tw + 32) / 2), y, tw + 32, 20};
    c.fillRound(pill, 10, withAlpha(col, 0.15f));
    dot(pill.x + 13, pill.cy(), col, 3.5f);
    c.text(t, {pill.x + 22, pill.y, tw + 4, pill.h}, 11, Bold, col);
  }
  y += 20 + 14;
  if (!g.measuring) c.text(L"SZOBAKÓD", {x0, y, cw, 14}, 11, Semibold, cMuted, Center);
  y += 14;
  if (!g.measuring) c.monoText(g.roomCode.empty() ? L"······" : g.roomCode, {x0, y, cw, 52}, 40, Bold, cText, Center);
  y += 52 + 4;
  if (!g.measuring) c.text(g.roomLink.empty() ? L"A link a kapcsolódás után jelenik meg…" : toW(g.roomLink), {x0, y, cw, 16}, 12, Regular, cMuted, Center);
  y += 16 + 14;
  bool copied = A().now() - g.copiedAt < 2000;
  if (copied && !g.measuring) A().keepGoing();
  button(B_COPY, {std::floor(W / 2 - 100), y, 200, 30}, copied ? L"Kimásolva" : L"Link másolása", copied ? Style::Success : Style::Secondary, true,
         copied ? Icon::Check : Icon::Copy, !g.roomLink.empty());
  y += 30 + 24;
  // how it goes
  float ih = 0;
  c.measure(g.info.empty() ? L" " : g.info, 11.5f, Regular, cw - 24, &ih);
  float sh = 40 + ih + 18;
  group({x0, y, cw, sh}, {y + 40});
  if (!g.measuring) {
    if (g.statusKind.empty()) spinner(x0 + 20, y + 20, 6.5f, cMuted);
    else dot(x0 + 20, y + 20, g.statusKind == "ok" ? cOk : g.statusKind == "warn" ? cWarn : g.statusKind == "error" ? cErr : cMuted);
    c.text(g.statusText, {x0 + 36, y, cw - 48, 40}, 13, Regular, g.statusKind == "error" ? cErr : cText);
    c.text(g.info, {x0 + 12, y + 40 + 9, cw - 24, ih + 2}, 11.5f, Regular, cMuted, Left, false, true);
  }
  y += sh + 22;
  // the viewers
  int count = int(g.viewers.size());
  if (!g.measuring) {
    c.text(L"Nézők", {x0 + 4, y, 200, 18}, 13, Semibold, cText);
    if (count) c.text(std::to_wstring(count), {x0 + cw - 104, y, 100, 18}, 12, Regular, cMuted, Right);
  }
  y += 18 + 7;
  if (count == 0) {
    group({x0, y, cw, 56});
    if (!g.measuring) c.text(L"Még nincs néző – küldd el a linket vagy a kódot.", {x0, y, cw, 56}, 12.5f, Regular, cMuted, Center);
    y += 56;
  } else {
    float rh = 46;
    group({x0, y, cw, rh * float(count)});
    int i = 0;
    for (const Json& v : g.viewers.items()) {
      Rect r{x0, y + rh * float(i), cw, rh};
      if (i > 0 && !g.measuring) c.fillRect({x0 + 12, std::floor(r.y), cw - 24, 1}, rgb(0xFFFFFF, 0.09f));
      viewerRow(v, r);
      i++;
    }
    y += rh * float(count);
  }
  y += 22;
  // pause, stop
  float pw = 130, sw = 170, gap = 10, bx = std::floor(W / 2 - (pw + gap + sw) / 2);
  button(B_PAUSE, {bx, y, pw, 30}, g.live ? L"Szünet" : L"Folytatás", Style::Secondary, true, g.live ? Icon::Pause : Icon::Play);
  button(B_STOP, {bx + pw + gap, y, sw, 30}, L"Megosztás vége", Style::Danger);
  y += 30;
  return y + kBottomPad;
}

const float kBarH = 52;

/** The viewer's bar: where its parts are (the drawing and the volume drag use the same numbers). */
struct BarLayout {
  Rect leave, full, volume, mute, more;
  float textRight;
};

BarLayout barLayout(float W) {
  BarLayout b;
  float right = W - 12;
  b.leave = {right - 104, 11, 104, 30};
  right -= 104 + 10;
  b.full = {right - 34, 11, 34, 30};
  right -= 34 + 14;
  b.volume = {right - 96, kBarH / 2 - 2, 96, 4};
  right -= 96 + 6;
  b.mute = {right - 34, 11, 34, 30};
  right -= 34 + 8;
  b.more = {right - 34, 11, 34, 30};
  b.textRight = right - 34 - 12;
  return b;
}

float drawViewing(float W, float H) {
  Canvas& c = C();
  float barH = g.fullscreen ? 0 : kBarH;
  if (g.measuring) return H;
  c.fillRect({0, barH, W, H - barH}, rgb(0x000000));
  if (!g.fullscreen) {
    BarLayout b = barLayout(W);
    c.verticalGradient({0, 0, W, barH}, cWin1, cWin2);
    c.fillRect({0, barH - 1, W, 1}, rgb(0xFFFFFF, 0.08f));
    dot(22, barH / 2, g.picture ? cOk : cWarn);
    button(B_LEAVE, b.leave, L"Kilépés", Style::Danger);
    button(B_FULL, b.full, L"", Style::Ghost, true, Icon::FullScreen);
    tip(B_FULL, L"Teljes képernyő (F)");
    button(B_NEW, b.more, L"", Style::Ghost, true, Icon::Plus);
    tip(B_NEW, L"Új ablak – még egy szoba nézése (Ctrl+N)");
    // volume: the icon mutes, the bar sets it
    hit(B_VOLUME, {b.volume.x - 8, 12, b.volume.w + 16, 28});
    float vol = A().to("vol", g.muted ? 0.0f : g.volume, 90);
    float vh = A().to(key("h", B_VOLUME), g.hover == B_VOLUME || g.draggingVolume ? 1.0f : 0.0f, 120);
    c.fillRound(b.volume, 2, rgb(0xFFFFFF, 0.18f));
    c.fillRound({b.volume.x, b.volume.y, std::max(4.0f, b.volume.w * vol), b.volume.h}, 2, cAccent);
    c.fillCircle(b.volume.x + b.volume.w * vol, b.volume.cy(), 6 + 1.2f * vh, cWhite);
    button(B_MUTE, b.mute, L"", Style::Ghost, true, g.muted ? Icon::Mute : Icon::Volume);
    tip(B_MUTE, g.muted ? L"Hang vissza (M)" : L"Némítás (M)");
    std::wstring head = L"Szoba: " + g.roomCode;
    c.text(head, {38, 0, 220, barH}, 13, Semibold, cText);
    float hw = c.measure(head, 13, Semibold);
    c.text(g.viewInfo, {38 + hw + 14, 0, std::max(0.0f, b.textRight - (38 + hw + 14)), barH}, 12, Regular, cMuted);
  }
  if (!g.picture || g.transitioning || !g.player || !g.player->hasPicture()) {
    float cy = barH + (H - barH) / 2;
    spinner(W / 2, cy - 28, 15, cMuted);
    c.text(g.overlayText, {40, cy + 2, W - 80, 60}, 14, Medium, withAlpha(cText, 0.9f), Center, false, true);
  }
  return H;
}

/** The About window, like a Mac's: the icon, the name, the version (build), the copyright; updates
 *  and the site under them. */
void drawAboutWindow(float W, float H) {
  Canvas& c = C();
  c.verticalGradient({0, 0, W, H}, cWin1, cWin2);
  float y = 26;
  c.appIcon({std::floor(W / 2 - 38), y, 76, 76});
  y += 76 + 16;
  c.text(L"Screen Share", {0, y, W, 24}, 17, Bold, cWhite, Center);
  y += 26;
  c.text(std::wstring(L"Verzió ") + kVersion + L" (" + kBuild + L")", {0, y, W, 18}, 12, Regular, cMuted, Center);
  y += 30;
  float th = 0;
  c.measure(kCopyright, 11.5f, Regular, W - 56, &th);
  c.text(kCopyright, {28, y, W - 56, th + 4}, 11.5f, Regular, cFaint, Center, false, true);
  y += th + 22;
  Rect r{std::floor(W / 2 - 105), y, 210, 30};
  if (g.updating && g.updateProgress >= 0) {
    float k = A().to("updateBar", float(g.updateProgress) / 100.0f, 250);
    c.fillRound(r, r.h / 2, rgb(0xFFFFFF, 0.12f));
    c.fillRound({r.x, r.y, std::max(r.h, r.w * k), r.h}, r.h / 2, cAccent);
    c.text(L"Letöltés… " + std::to_wstring(g.updateProgress) + L"%", r, 12.5f, Semibold, cWhite, Center);
  } else if (!g.update.version.empty()) {
    button(B_UPDATE, r, g.updating ? L"Telepítés…" : L"Frissítés: " + toW(g.update.version), Style::Primary, true, Icon::Download, !g.updating);
  } else {
    button(B_CHECK, r, g.checking ? L"Keresés…" : L"Frissítések keresése", Style::Secondary, true, Icon::Refresh, !g.checking);
  }
  y += 30 + 10;
  c.text(g.updateNote, {0, y, W, 18}, 12, Regular, g.update.version.empty() ? cMuted : cOk, Center);
  y += 24;
  button(B_SITE, {std::floor(W / 2 - 70), y, 140, 28}, L"mozizas.hu", Style::Ghost, true, Icon::Link);
  y += 40;
  const wchar_t* credits = L"BearSSL (MIT) · Opus (BSD) · NVIDIA Video Codec SDK (MIT)";
  c.measure(credits, 10.5f, Regular, W - 40, &th);
  c.text(credits, {20, y, W - 40, th + 4}, 10.5f, Regular, cFaint, Center, false, true);
}

/** A question over the screen (the Mac's alert): the title, the text, the buttons. */
void drawModal(float W, float H) {
  Canvas& c = C();
  float k = A().to("modal", g.modal.open ? 1.0f : 0.0f, 170);
  if (k < 0.005f) return;
  if (g.modal.open) g.hits.clear(); // only the dialog answers
  c.pushAlpha(k);
  c.fillRect({0, 0, W, H}, rgb(0x000000, 0.45f));
  float w = std::min(360.0f, W - 40), th = 0;
  c.measure(g.modal.text, 13, Regular, w - 48, &th);
  float h = 24 + 22 + 8 + th + 22 + 30 + 22;
  Rect r{std::floor((W - w) / 2), std::floor((H - h) / 2), w, h};
  c.pushScale(0.96f + 0.04f * k, r.cx(), r.cy());
  c.shadow(r, 14, 1.4f);
  c.fillRound(r, 14, rgb(0x2E2E33));
  c.strokeRound(r, 14, rgb(0xFFFFFF, 0.12f), 1);
  float x = r.x + 24, y = r.y + 24;
  c.text(g.modal.title, {x, y, w - 48, 22}, 15, Semibold, cText);
  y += 22 + 8;
  c.text(g.modal.text, {x, y, w - 48, th + 4}, 13, Regular, cMuted, Left, false, true);
  y += th + 22;
  float bw = (w - 48 - 10) / 2;
  if (g.modal.cancel.empty()) {
    button(B_MODAL_OK, {x, y, w - 48, 30}, g.modal.ok, Style::Primary);
  } else {
    button(B_MODAL_CANCEL, {x, y, bw, 30}, g.modal.cancel, Style::Secondary);
    button(B_MODAL_OK, {x + bw + 10, y, bw, 30}, g.modal.ok, g.modal.danger ? Style::Danger : Style::Primary);
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

/* ------------------------------------------------ the window: as tall as what it shows (as the Mac's) */

const UINT WM_FIT = WM_APP + 5;

float screenWidth(Screen s) { return (s == Screen::Hosting ? kHostW : kHomeW) + 2 * kSide; }

/** How tall a compact screen's content is (laid out, not drawn). */
float measureScreen(Screen s) {
  bool was = g.measuring;
  g.measuring = true;
  float h = drawScreen(s, screenWidth(s), 0);
  g.measuring = was;
  return h;
}

/** The compact window to the size of its content: its top centre stays (or goes back where it was
 *  before watching), and it stays on the screen. */
void fitWindow() {
  if (g.screen == Screen::Viewing || g.fullscreen || IsIconic(g.wnd) || IsZoomed(g.wnd)) return;
  RECT want{0, 0, px(screenWidth(g.screen)), px(g.contentHeight)};
  AdjustWindowRectExForDpi(&want, DWORD(GetWindowLongPtrW(g.wnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongPtrW(g.wnd, GWL_EXSTYLE)), g.dpi);
  int w = want.right - want.left, h = want.bottom - want.top;
  RECT cur;
  GetWindowRect(g.wnd, &cur);
  POINT top = g.anchored ? g.anchor : POINT{(cur.left + cur.right) / 2, cur.top};
  g.anchored = false;
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromPoint(top, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT& wa = mi.rcWork;
  w = std::min<int>(w, wa.right - wa.left);
  h = std::min<int>(h, wa.bottom - wa.top);
  int x = std::clamp<int>(top.x - w / 2, wa.left, wa.right - w), y = std::clamp<int>(top.y, wa.top, wa.bottom - h);
  if (x == cur.left && y == cur.top && w == cur.right - cur.left && h == cur.bottom - cur.top) return;
  SetWindowPos(g.wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/** After something that changes the layout (a tab, an error, a viewer): measured and fitted now. */
void relayout() {
  if (g.screen != Screen::Viewing) {
    g.contentHeight = measureScreen(g.screen);
    fitWindow();
  }
  animate();
}

/** Watching: a large window that can be resized (as last time, or 16:9 on most of the screen). */
void enterViewerWindow() {
  RECT wr;
  GetWindowRect(g.wnd, &wr);
  g.anchor = {(wr.left + wr.right) / 2, wr.top};
  g.anchored = true;
  SetWindowLongPtrW(g.wnd, GWL_STYLE, GetWindowLongPtrW(g.wnd, GWL_STYLE) | WS_THICKFRAME | WS_MAXIMIZEBOX);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT& wa = mi.rcWork;
  int maxW = wa.right - wa.left, maxH = wa.bottom - wa.top;
  int w = int(loadDword(L"ViewerW", 0)), h = int(loadDword(L"ViewerH", 0));
  if (w < px(640) || h < px(400) || w > maxW || h > maxH) {
    w = std::min(px(1280), maxW * 86 / 100);
    h = w * 9 / 16 + px(kBarH) + GetSystemMetricsForDpi(SM_CYCAPTION, g.dpi);
    if (h > maxH * 86 / 100) {
      h = maxH * 86 / 100;
      w = (h - px(kBarH)) * 16 / 9;
    }
  }
  int x = std::clamp<int>((wr.left + wr.right) / 2 - w / 2, wa.left, wa.right - w), y = std::clamp<int>((wr.top + wr.bottom) / 2 - h / 2, wa.top, wa.bottom - h);
  SetWindowPos(g.wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
  if (loadDword(L"ViewerMax", 0)) ShowWindow(g.wnd, SW_MAXIMIZE);
}

/** Back from watching: the viewer's size is kept for next time, the window compact again. */
void leaveViewerWindow() {
  bool zoomed = IsZoomed(g.wnd) != FALSE;
  saveDword(L"ViewerMax", zoomed);
  if (zoomed) ShowWindow(g.wnd, SW_RESTORE);
  RECT wr;
  GetWindowRect(g.wnd, &wr);
  saveDword(L"ViewerW", DWORD(wr.right - wr.left));
  saveDword(L"ViewerH", DWORD(wr.bottom - wr.top));
  SetWindowLongPtrW(g.wnd, GWL_STYLE, GetWindowLongPtrW(g.wnd, GWL_STYLE) & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
  SetWindowPos(g.wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
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
    int x = px(r.x + 8), w = px(r.w - 16), eh = tm.tmHeight + 2;
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
  placeChild(ID_PASSWORD, g.pass, home && g.tab == 0);
  placeChild(ID_CODE, g.code, home && g.tab == 1);
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
  g.tips.clear();
  float W = c.width(), H = c.height();
  float t = 1;
  if (g.transitioning) {
    t = easeOut(float((A().now() - g.transitionStart) / 240.0));
    if (t >= 1.0f) g.transitioning = false;
  }
  if (g.screen == Screen::Viewing && !g.transitioning) c.clear(rgb(0x000000));
  else c.verticalGradient({0, 0, W, H}, cWin1, cWin2);
  if (g.transitioning) {
    // the screens cross-fade: the old one rises and fades, the new one comes up
    c.pushAlpha(1 - t);
    c.pushTranslate(0, -10 * t);
    drawScreen(g.prev, W, H);
    c.popTranslate();
    c.popAlpha();
    c.pushAlpha(t);
    c.pushTranslate(0, 12 * (1 - t));
    g.contentHeight = drawScreen(g.screen, W, H);
    c.popTranslate();
    c.popAlpha();
    A().keepGoing();
  } else {
    // the scroll position (when the content is taller than the screen): eased, kept within it
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
  drawTip(W, H);
  if (g.screen == Screen::Viewing && g.picture && g.player && !g.player->hasPicture()) A().keepGoing(); // until the first frame shows
  c.end();
  updateChildren();
  // the compact window follows its content (once the cross-fade is over)
  if (!g.transitioning && g.screen != Screen::Viewing && !g.fitPosted && (std::fabs(g.contentHeight - H) > 0.75f || std::fabs(screenWidth(g.screen) - W) > 0.75f)) {
    g.fitPosted = true;
    PostMessageW(g.wnd, WM_FIT, 0, 0);
  }
  if (A().active()) {
    animate();
  } else if (g.animTimer) {
    KillTimer(g.wnd, TIMER_ANIM);
    g.animTimer = false;
  }
}

void show(Screen s) {
  if (s == g.screen) return;
  bool fromViewer = g.screen == Screen::Viewing;
  g.prev = g.screen;
  g.screen = s;
  g.transitioning = true;
  g.transitionStart = nowMs();
  g.scrollTarget = 0;
  A().set("scroll", 0);
  g.scroll = 0;
  g.hover = g.pressed = ID_NONE;
  if (s == Screen::Viewing) {
    enterViewerWindow();
  } else {
    if (fromViewer) leaveViewerWindow();
    g.contentHeight = measureScreen(s);
    fitWindow();
  }
  updateChildren(); // the text boxes and the picture hide while the screens cross-fade
  if (s != Screen::Home) SetFocus(g.wnd);
  animate();
}

/** Megosztás or Nézés on the home screen. */
void setTab(int tab) {
  if (tab == g.tab) return;
  g.tab = tab;
  saveDword(L"Tab", DWORD(tab));
  A().set("pane", 0);
  g.hover = ID_NONE;
  relayout();
  updateChildren();
  if (tab == 1) SetFocus(g.code);
  else if (GetWindowTextLengthW(g.pass) == 0) SetFocus(g.pass);
  else SetFocus(g.wnd);
}

/** A pop-up's menu, under it (Windows' own, dark): the choice is kept. */
void openMenu(int id) {
  auto it = g.controlRects.find(id);
  if (it == g.controlRects.end()) return;
  Rect r = it->second;
  HMENU m = CreatePopupMenu();
  if (id == B_MONITOR) {
    for (size_t i = 0; i < g.monitors.size(); i++) AppendMenuW(m, MF_STRING | (int(i) == g.monitor ? MF_CHECKED : 0), kMenuMonitor0 + UINT(i), g.monitors[i].name.c_str());
  } else {
    for (size_t i = 0; i < std::size(kQualities); i++) AppendMenuW(m, MF_STRING | (int(i) == g.quality ? MF_CHECKED : 0), kMenuQuality0 + UINT(i), kQualities[i].name);
  }
  POINT pt{px(r.x), px(r.bottom() - g.scroll + 3)};
  ClientToScreen(g.wnd, &pt);
  g.pressed = g.hover = ID_NONE;
  invalidate();
  UINT cmd = UINT(TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, g.wnd, nullptr));
  DestroyMenu(m);
  if (cmd >= kMenuQuality0 && cmd < kMenuQuality0 + std::size(kQualities)) {
    g.quality = int(cmd - kMenuQuality0);
    saveDword(L"Quality", DWORD(g.quality));
  } else if (cmd >= kMenuMonitor0 && cmd < kMenuMonitor0 + g.monitors.size()) {
    g.monitor = int(cmd - kMenuMonitor0);
    saveDword(L"Monitor", DWORD(g.monitor));
  }
  relayout(); // (another hint)
}

/* ------------------------------------------------ more windows: each its own process, a little offset */

/** Another window (to watch one more room – on its Nézés tab): this program once more, near this window. */
void newWindow(bool watch = true) {
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
  RECT wr;
  GetWindowRect(g.wnd, &wr);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --from " + std::to_wstring(wr.left) + L" " + std::to_wstring(wr.top) + (watch ? L" --watch" : L"");
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

/** The title (on the taskbar, Alt+Tab): which room this window shows. */
void setTitle(const std::wstring& what) {
  SetWindowTextW(g.wnd, what.empty() ? L"Screen Share" : (what + L" – Screen Share").c_str());
}

/** The other Screen Share windows (other processes of this program). */
std::vector<HWND> otherWindows() {
  std::vector<HWND> out;
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[32];
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId() && GetClassNameW(h, cls, 32) && std::wstring(cls) == L"ScreenShare") reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
      },
      LPARAM(&out));
  return out;
}

/** The taskbar button's menu (right click): Új ablak. */
void setJumpList() {
  using Microsoft::WRL::ComPtr;
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
  ComPtr<ICustomDestinationList> list;
  ComPtr<IObjectCollection> tasks;
  ComPtr<IShellLinkW> link;
  ComPtr<IPropertyStore> props;
  ComPtr<IObjectArray> removed, array;
  UINT slots = 0;
  if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))) ||
      FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks))) ||
      FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&props)) ||
      FAILED(list->BeginList(&slots, IID_PPV_ARGS(&removed))))
    return;
  link->SetPath(exe);
  link->SetArguments(L"--watch");
  link->SetIconLocation(exe, 0);
  link->SetDescription(L"Még egy ablak – például egy másik szoba nézéséhez");
  // the task's name (PKEY_Title)
  const PROPERTYKEY title = {{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
  PROPVARIANT v;
  PropVariantInit(&v);
  v.vt = VT_LPWSTR;
  const wchar_t* name = L"Új ablak";
  size_t bytes = (wcslen(name) + 1) * sizeof(wchar_t);
  v.pwszVal = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
  if (!v.pwszVal) return;
  std::memcpy(v.pwszVal, name, bytes);
  props->SetValue(title, v);
  props->Commit();
  PropVariantClear(&v);
  if (SUCCEEDED(tasks->AddObject(link.Get())) && SUCCEEDED(tasks.As(&array)) && SUCCEEDED(list->AddUserTasks(array.Get()))) list->CommitList();
  else list->AbortList();
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
  relayout();
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
  saveDword(L"Remember", g.remember);
  savePassword(g.remember ? pw : L"");
  g.shareErr.clear();
  endSession();
  int session = ++g.session;
  EngineConfig cfg;
  cfg.server = g.server;
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
  setTitle(L"Megosztás");
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
  cfg.server = g.server;
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
  setTitle(L"Szoba " + g.roomCode);
  show(Screen::Viewing);
  engine->join(code);
}

void toggleFullscreen();

void stopToHome(const std::wstring& message = L"") {
  if (g.fullscreen) toggleFullscreen();
  endSession();
  setTitle(L"");
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
    setTitle(L"Megosztás · " + g.roomCode);
    if (e["changed"].b()) ask(L"Új link készült", L"A korábbi szoba lejárt, ezért új kód és link készült – küldd el újra a nézőknek.", L"Rendben", L"", false, nullptr);
  } else if (type == "host-error") {
    std::string c = e["code"].str();
    std::wstring text = c == "auth" ? L"Hibás megosztási jelszó." : c == "rate" ? L"Túl sok próbálkozás, várj egy kicsit." : c == "full" ? L"A szerver megtelt, próbáld később."
                                                                                                                                        : L"Nem érem el a szervert (" + toW(c) + L").";
    endSession();
    setTitle(L"");
    show(Screen::Home);
    setError(g.shareErr, text, "shareErr");
  } else if (type == "viewers") {
    for (const Json& v : e["list"].items()) {
      std::string id = "v" + v["id"].str("");
      if (A().get(id, -1) < 0) A().set(id, 0); // a new viewer fades in
    }
    std::vector<Json> list = e["list"].items();
    std::sort(list.begin(), list.end(), [](const Json& a, const Json& b) { return a["n"].num() < b["n"].num(); });
    bool changed = list.size() != g.viewers.size();
    g.viewers = Json(list);
    if (changed) relayout();
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

void installUpdate(bool othersAsked = false) {
  if (g.updating || g.update.version.empty()) return;
  if (!othersAsked && !otherWindows().empty()) { // the new version replaces the program: the other windows close
    ask(L"Frissítés", L"A frissítéshez a többi Screen Share ablak is bezárul – a bennük futó nézés vagy megosztás véget ér.", L"Frissítés", L"Mégse", false, [] {
      for (HWND h : otherWindows()) PostMessageW(h, WM_CLOSE_FOR_UPDATE, 0, 0);
      installUpdate(true);
    });
    return;
  }
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
  switch (id) {
    case B_TAB0: setTab(0); break;
    case B_NEW: newWindow(); break;
    case B_TAB1: setTab(1); break;
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
  g.editFont = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
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
  g.pass = makeEdit(ID_PASSWORD, ES_PASSWORD, pw, L"megosztási jelszó");
  g.code = makeEdit(ID_CODE, 0, L"", L"abc123 vagy link");
  g.video = CreateWindowExW(0, L"SSVideo", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, g.wnd, nullptr, g.inst, nullptr);
  g.remember = loadDword(L"Remember", 1) != 0; // (on unless turned off, as on the Mac)
  g.audio = loadDword(L"Audio", 1) != 0;
  g.quality = int(std::min<DWORD>(loadDword(L"Quality", 0), DWORD(std::size(kQualities) - 1)));
  g.monitors = listMonitors();
  g.monitor = int(std::min<DWORD>(loadDword(L"Monitor", 0), DWORD(g.monitors.empty() ? 0 : g.monitors.size() - 1)));
  g.volume = float(std::min<DWORD>(loadDword(L"Volume", 100), 100)) / 100.0f;
  g.tab = loadDword(L"Tab", 0) == 1 ? 1 : 0;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g.wnd = h;
      g.dpi = GetDpiForWindow(h);
      createControls();
      if (HMENU sys = GetSystemMenu(h, FALSE)) {
        AppendMenuW(sys, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sys, MF_STRING, kSysNew, L"Új ablak\tCtrl+N");
        AppendMenuW(sys, MF_STRING, kSysAbout, L"A Screen Share névjegye");
      }
      SetTimer(h, TIMER_TICK, 1000, nullptr);
      if (!g.shots) checkUpdatesLater();
      return 0;
    case WM_DPICHANGED: {
      g.dpi = HIWORD(wp);
      makeEditFont();
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      relayout();
      return 0;
    }
    case WM_SIZE:
      g.placed.erase(-1);
      invalidate();
      return 0;
    case WM_FIT:
      g.fitPosted = false;
      fitWindow();
      return 0;
    case WM_CLOSE_FOR_UPDATE:
      DestroyWindow(h); // (the session ends there)
      return 0;
    case WM_GETMINMAXINFO: {
      if (g.screen == Screen::Viewing) reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {px(640), px(400)};
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
        g.hoverSince = nowMs();
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
      if (g.pressed == B_MONITOR || g.pressed == B_QUALITY) { // a pop-up's menu opens on the press
        SetFocus(h);
        openMenu(g.pressed);
        return 0;
      }
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
      if ((wp & 0xFFF0) == kSysNew) {
        newWindow();
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

/* ------------------------------------------------ dark menus (the pop-ups', the window menu) */

/** Windows' own menus in the dark: uxtheme's SetPreferredAppMode (ordinal 135 – not documented, but
 *  what the system's own apps use; from Windows 10 1903, before which 135 was something else). */
void darkMenus() {
  typedef LONG(WINAPI * GetVersionFn)(OSVERSIONINFOW*);
  typedef int(WINAPI * SetModeFn)(int);
  typedef void(WINAPI * FlushFn)();
  GetVersionFn version = nullptr;
  SetModeFn setMode = nullptr;
  FlushFn flush = nullptr;
  FARPROC p = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  std::memcpy(&version, &p, sizeof version);
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof v;
  if (!version || version(&v) != 0 || v.dwBuildNumber < 18362) return;
  HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!ux) return;
  p = GetProcAddress(ux, MAKEINTRESOURCEA(135));
  std::memcpy(&setMode, &p, sizeof setMode);
  p = GetProcAddress(ux, MAKEINTRESOURCEA(136));
  std::memcpy(&flush, &p, sizeof flush);
  if (setMode) setMode(2); // ForceDark
  if (flush) flush();
}

/** The compact window in the middle of its screen, a little above (as the Mac's). */
void centerWindow() {
  RECT want{0, 0, px(screenWidth(g.screen)), px(g.contentHeight)};
  AdjustWindowRectExForDpi(&want, DWORD(GetWindowLongPtrW(g.wnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongPtrW(g.wnd, GWL_EXSTYLE)), g.dpi);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
  const RECT& wa = mi.rcWork;
  int w = std::min<int>(want.right - want.left, wa.right - wa.left), h = std::min<int>(want.bottom - want.top, wa.bottom - wa.top);
  SetWindowPos(g.wnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 3, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------ screenshots (CI): every screen drawn and saved */

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

bool savePng(HDC dc, HBITMAP bmp, int w, int h, const std::wstring& path) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  std::vector<BYTE> pixels(size_t(w) * size_t(h) * 4);
  if (!GetDIBits(dc, bmp, 0, UINT(h), pixels.data(), &bi, DIB_RGB_COLORS)) return false;
  for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && SUCCEEDED(wic->CreateStream(&stream)) &&
         SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) && SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
         SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->SetSize(UINT(w), UINT(h))) && SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
         SUCCEEDED(frame->WritePixels(UINT(h), UINT(w) * 4, UINT(pixels.size()), pixels.data())) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

/** The window as Windows composes it (the title bar, what Direct2D drew, the text boxes) into a PNG. */
bool shoot(HWND h, const std::wstring& path) {
  RECT wr, vis;
  GetWindowRect(h, &wr);
  if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof vis))) vis = wr; // (without the invisible borders)
  int w = wr.right - wr.left, hh = wr.bottom - wr.top, cw = vis.right - vis.left, ch = vis.bottom - vis.top;
  if (w <= 0 || hh <= 0 || cw <= 0 || ch <= 0) return false;
  HDC screen = GetDC(nullptr);
  HDC whole = CreateCompatibleDC(screen), part = CreateCompatibleDC(screen);
  HBITMAP wb = CreateCompatibleBitmap(screen, w, hh), pb = CreateCompatibleBitmap(screen, cw, ch);
  HGDIOBJ o1 = SelectObject(whole, wb), o2 = SelectObject(part, pb);
  bool ok = PrintWindow(h, whole, PW_RENDERFULLCONTENT) != FALSE;
  BitBlt(part, 0, 0, cw, ch, whole, vis.left - wr.left, vis.top - wr.top, SRCCOPY);
  SelectObject(whole, o1);
  SelectObject(part, o2);
  ok = savePng(part, pb, cw, ch, path) && ok;
  DeleteObject(wb);
  DeleteObject(pb);
  DeleteDC(whole);
  DeleteDC(part);
  ReleaseDC(nullptr, screen);
  return ok;
}

void pumpFor(double ms) {
  double end = nowMs() + ms;
  MSG m;
  while (nowMs() < end) { // (the clock checked after each message: while something moves, a paint is always due)
    if (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    } else {
      Sleep(5);
    }
  }
}

/** --ui-shot <folder>: each screen as it looks with made-up content, saved as a PNG, and whether
 *  Direct2D drew it (a frame it refuses – a resource of another factory, say – fails: exit 1). */
int uiShots(const std::wstring& dir) {
  CreateDirectoryW(dir.c_str(), nullptr);
  double hold = 900; // (SS_SHOT_HOLD: longer, for a screenshot from outside, as under Wine)
  if (const char* e = std::getenv("SS_SHOT_HOLD")) hold = std::max(100.0, std::atof(e));
  std::string report;
  bool failed = false;
  auto snap = [&](const wchar_t* name, HWND h) {
    pumpFor(hold);
    bool saved = h && shoot(h, dir + L"\\" + name + L".png");
    HRESULT drawn = (h && h == g.about ? g.aboutCanvas : g.canvas).lastResult();
    char line[200];
    std::snprintf(line, sizeof line, "%s: %s, drawn 0x%08lX\n", narrow(name).c_str(), saved ? "saved" : "not saved", static_cast<unsigned long>(drawn));
    report += line;
    if (FAILED(drawn)) failed = true;
  };
  SetWindowTextW(g.pass, L"titok123");
  snap(L"1-home-share", g.wnd);
  setTab(1);
  snap(L"2-home-watch", g.wnd);
  setTab(0);
  setError(g.shareErr, L"Hibás megosztási jelszó.", "shareErr");
  snap(L"3-home-error", g.wnd);
  g.shareErr.clear();
  // sharing, with two viewers
  g.live = true;
  g.roomCode = L"x7k2pd";
  g.roomLink = "https://mozizas.hu/#x7k2pd";
  g.statusText = L"Kapcsolódva. A nézők a linkkel vagy a kóddal csatlakozhatnak.";
  g.statusKind = "ok";
  g.info = L"Kép: 2560×1440 · NVENC (videókártya) · 60 FPS · 2,1 ms/kép · hang: sztereó";
  g.viewers = Json::parse(R"([{"id":"a","n":1,"state":"connected","path":"lan","fps":60,"kbps":18400,"rtt":3,"lat":21},{"id":"b","n":2,"state":"connecting"}])");
  show(Screen::Hosting);
  snap(L"4-hosting", g.wnd);
  ask(L"Befejezed a megosztást?", L"A nézőknél megáll a kép, és a szoba bezárul.", L"Befejezés", L"Mégse", true, nullptr);
  snap(L"5-question", g.wnd);
  closeModal(false);
  g.viewers = Json::array();
  g.statusKind.clear();
  g.statusText = L"Kapcsolódás a szerverhez…";
  relayout();
  snap(L"6-hosting-empty", g.wnd);
  // watching, before the picture comes
  g.overlayText = L"Csatlakozás a szobához…";
  g.viewInfo = L"Várakozás a képre…";
  show(Screen::Viewing);
  snap(L"7-viewing", g.wnd);
  show(Screen::Home);
  pumpFor(300);
  showAbout();
  snap(L"8-about", g.about);
  if (FILE* f = _wfopen((dir + L"\\ui-check.txt").c_str(), L"w")) {
    std::fputs(report.c_str(), f);
    std::fclose(f);
  }
  DestroyWindow(g.wnd);
  return failed ? 1 : 0;
}

/** What a part of the screen shows (as Windows composed it: what a person sees there). */
bool grabScreen(const RECT& r, std::vector<BYTE>& pixels) {
  int w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return false;
  HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  bool ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
  SelectObject(mem, old);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  pixels.assign(size_t(w) * size_t(h) * 4, 0);
  ok = ok && GetDIBits(mem, bmp, 0, UINT(h), pixels.data(), &bi, DIB_RGB_COLORS);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok;
}

/** --ui-watch <server> <code> <folder> (CI): the app watches a room as a person would – its window,
 *  the video in it – and the screen there is looked at: the exit code says whether the picture
 *  shows (the test stream is colour bars: most of it bright), the window is saved to look at. */
int uiWatch(const std::string& server, const std::string& code, const std::wstring& dir) {
  CreateDirectoryW(dir.c_str(), nullptr);
  g.server = server;
  startWatching(code);
  double until = nowMs() + 25000;
  while (nowMs() < until && !(g.picture && g.player && g.player->hasPicture())) pumpFor(100);
  bool pictured = g.player && g.player->hasPicture();
  pumpFor(2500); // more frames; the fade-in over
  SetWindowPos(g.wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  SetForegroundWindow(g.wnd);
  pumpFor(600);
  // the video's area: the window's client below the bar, on the screen
  RECT rc;
  GetClientRect(g.wnd, &rc);
  POINT tl{0, px(kBarH)}, br{rc.right, rc.bottom};
  ClientToScreen(g.wnd, &tl);
  ClientToScreen(g.wnd, &br);
  RECT area{tl.x + 8, tl.y + 8, br.x - 8, br.y - 8};
  std::vector<BYTE> pixels;
  bool grabbed = grabScreen(area, pixels);
  size_t bright = 0, total = pixels.size() / 4;
  for (size_t i = 0; i < pixels.size(); i += 4) bright += std::max({pixels[i], pixels[i + 1], pixels[i + 2]}) > 60 ? 1 : 0;
  double share = total ? double(bright) / double(total) : 0;
  shoot(g.wnd, dir + L"\\watch.png");
  VideoPlayer::Stats st = g.player ? g.player->stats() : VideoPlayer::Stats{};
  char report[400];
  std::snprintf(report, sizeof report, "picture %d, shown %d, grabbed %d, bright %.3f, %dx%d, %.0f FPS, decoding %s, latency %.1f ms\n", g.picture ? 1 : 0, pictured ? 1 : 0,
                grabbed ? 1 : 0, share, st.width, st.height, st.fps, st.hardware ? "GPU" : "CPU", st.latencyMs);
  if (FILE* f = _wfopen((dir + L"\\ui-watch.txt").c_str(), L"w")) {
    std::fputs(report, f);
    std::fclose(f);
  }
  endSession();
  DestroyWindow(g.wnd);
  return pictured && share > 0.3 ? 0 : 1;
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
  if (argc == 4 && std::wstring(argv[1]) == L"--ws-bench") { // CI: round trips through this WebSocket (WinHTTP)
    FILE* log = _wfopen(argv[3], L"w");
    if (!log) return 1;
    std::fprintf(log, "BENCH %s\n", wsBench(openWinHttpWebSocket, narrow(argv[2]), 240).dump().c_str());
    std::fclose(log);
    return 0;
  }
  g.shots = (argc == 3 && std::wstring(argv[1]) == L"--ui-shot") || (argc == 5 && std::wstring(argv[1]) == L"--ui-watch");
  darkMenus();
  // compact (as the Mac's): as tall as the home screen, not resized by hand; watching makes it large
  HWND wnd = CreateWindowExW(0, L"ScreenShare", L"Screen Share", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             480, 640, nullptr, nullptr, inst, nullptr);
  styleTitleBar(wnd);
  g.contentHeight = measureScreen(Screen::Home);
  centerWindow();
  if (g.shots) {
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);
    int result = argc == 5 ? uiWatch(narrow(argv[2]), narrow(argv[3]), argv[4]) : uiShots(argv[2]);
    timeEndPeriod(1);
    MFShutdown();
    CoUninitialize();
    return result;
  }
  // the command line: --from <x> <y> (another window's corner: this one goes near it), --watch
  // (the Nézés tab), a room link (screenshare://abc123, or the code)
  std::wstring link;
  bool watch = false, from = false;
  POINT at{};
  for (int i = 1; i < argc; i++) {
    std::wstring a = argv[i];
    if (a == L"--from" && i + 2 < argc) {
      at = {_wtoi(argv[i + 1]), _wtoi(argv[i + 2])};
      from = true;
      i += 2;
    } else if (a == L"--watch") {
      watch = true;
    } else if (!a.empty() && a[0] != L'-' && a[0] != L'/') {
      link = a;
    }
  }
  if (!from) { // (started from the taskbar's menu or a link while another window is open: near that one)
    std::vector<HWND> others = otherWindows();
    RECT r;
    if (!others.empty() && GetWindowRect(others.front(), &r)) {
      at = {r.left, r.top};
      from = true;
    }
  }
  if (from) {
    at.x += 32;
    at.y += 32;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    RECT r;
    GetWindowRect(wnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(wnd, nullptr, std::clamp<int>(at.x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - w)),
                 std::clamp<int>(at.y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - h)), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (watch) {
    g.tab = 1;
    g.contentHeight = measureScreen(Screen::Home);
    fitWindow();
  }
  ShowWindow(wnd, showCmd);
  UpdateWindow(wnd);
  setJumpList();
  if (watch) SetFocus(g.code);
  if (!link.empty()) {
    std::string code = parseCode(link);
    if (code.size() >= 4) startWatching(code);
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (msg.message == WM_KEYDOWN && msg.wParam == 'N' && (GetKeyState(VK_CONTROL) & 0x8000) && !g.modal.open) {
      newWindow(); // Ctrl+N: another window
      continue;
    }
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
