// Screen Share for Windows – native, without Electron or Chromium.
//
// Home: share the screen (sharing password, which screen, quality, system sound) or watch a room
// (code or link). Sharing: the room code and link, the viewers with their connection and latency.
// Watching: the picture fills the window (F / double click: full screen, S: statistics, M: mute).
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

#include <atomic>
#include <cstdio>
#include <cmath>
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
#include "update.h"

#ifndef SS_VERSION
#define SS_VERSION "2.0.0"
#endif

namespace ss {
std::unique_ptr<WebSocket> openWinHttpWebSocket(const std::string& url, WebSocketEvents events);
}

using namespace ss;

namespace {

const UINT WM_ENGINE = WM_APP + 1; // wParam: session, lParam: std::string* (an event as JSON)
const UINT WM_UPDATE = WM_APP + 2; // lParam: UpdateInfo*
const UINT WM_UPDATE_PROGRESS = WM_APP + 3;
const UINT WM_UPDATE_DONE = WM_APP + 4; // wParam: ok

const COLORREF kBg = RGB(0x14, 0x14, 0x1C), kCard = RGB(0x1D, 0x1D, 0x28), kText = RGB(0xEC, 0xEC, 0xF2), kMuted = RGB(0x9A, 0x9A, 0xAC),
               kAccent = RGB(0xFF, 0x4D, 0x5E), kEdit = RGB(0x26, 0x26, 0x33), kOk = RGB(0x5A, 0xD1, 0x8C), kWarn = RGB(0xFF, 0xB4, 0x4D);

enum Id {
  ID_PASSWORD = 100, ID_REMEMBER, ID_MONITOR, ID_QUALITY, ID_AUDIO, ID_SHARE, ID_CODE, ID_JOIN, ID_SERVER, ID_UPDATE,
  ID_COPY, ID_PAUSE, ID_STOP, ID_FULL, ID_LEAVE, ID_VOLUME, ID_STATS
};

struct Quality {
  const wchar_t* name;
  int w, h, fps;
};
const Quality kQualities[] = {
    {L"Automatikus – a legszebb (4K, 60 FPS)", 3840, 2160, 60},
    {L"1440p, 60 FPS", 2560, 1440, 60},
    {L"1080p, 60 FPS", 1920, 1080, 60},
    {L"1080p, 120 FPS (játék)", 1920, 1080, 120},
    {L"720p, 60 FPS (gyenge net)", 1280, 720, 60},
    {L"Eredeti felbontás, 60 FPS", 0, 0, 60},
};

enum class Screen { Home, Hosting, Viewing };

struct App {
  HINSTANCE inst = nullptr;
  HWND wnd = nullptr;
  UINT dpi = 96;
  HFONT font = nullptr, bold = nullptr, big = nullptr, title = nullptr, smallFont = nullptr;
  HBRUSH bgBrush = nullptr, cardBrush = nullptr, editBrush = nullptr;
  Screen screen = Screen::Home;

  // home
  HWND title1, sub1, shareHead, passLabel, pass, remember, monLabel, monitor, qualLabel, quality, audio, share, shareErr;
  HWND joinHead, codeLabel, code, join, joinErr, serverLabel, server, footer, updateBtn;
  // sharing
  HWND roomLabel, roomCode, link, copy, status, viewersHead, viewers, info, pause, stop;
  // watching
  HWND bar, viewInfo, full, leave, volume, video, overlay;

  std::vector<HWND> homeCtl, hostCtl, viewCtl;
  std::vector<Monitor> monitors;

  std::unique_ptr<Engine> engine;
  std::unique_ptr<HostPipeline> host;
  std::unique_ptr<VideoPlayer> player;
  std::unique_ptr<AudioPlayer> audioOut;
  std::atomic<int> session{0};
  bool live = true, picture = false, fullscreen = false, stats = false, muted = false;
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  LONG_PTR savedStyle = 0;
  std::string roomLink, viewerLines, statusText, statusKind, encoderName, overlayText;
  Json viewStats;
  UpdateInfo update;
  bool updating = false;
};

App g;

int px(int v) { return MulDiv(v, int(g.dpi), 96); }

std::wstring W(const std::string& s) { return widen(s); }

void setText(HWND h, const std::wstring& t) { SetWindowTextW(h, t.c_str()); }

std::wstring getText(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(size_t(n) + 1, L'\0');
  GetWindowTextW(h, &s[0], n + 1);
  s.resize(size_t(n));
  return s;
}

/* ------------------------------------------------ settings (HKCU\Software\ScreenShare) */

const wchar_t* kKey = L"Software\\ScreenShare";

std::wstring loadString(const wchar_t* name, const std::wstring& fallback) {
  wchar_t buf[1024];
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
  return fallback;
}

DWORD loadDword(const wchar_t* name, DWORD fallback) {
  DWORD v = 0, size = sizeof v;
  return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : fallback;
}

void saveString(const wchar_t* name, const std::wstring& v) {
  RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
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

/* ------------------------------------------------ controls */

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, std::vector<HWND>& group, DWORD ex = 0) {
  HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, g.wnd, reinterpret_cast<HMENU>(intptr_t(id)), g.inst, nullptr);
  SendMessageW(h, WM_SETFONT, WPARAM(g.font), TRUE);
  group.push_back(h);
  return h;
}

HWND label(const wchar_t* text, std::vector<HWND>& group, HFONT f = nullptr) {
  HWND h = make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, 0, group);
  if (f) SendMessageW(h, WM_SETFONT, WPARAM(f), TRUE);
  return h;
}

HWND button(const wchar_t* text, int id, std::vector<HWND>& group) { return make(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, id, group); }

HWND edit(int id, DWORD style, std::vector<HWND>& group) {
  HWND h = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | style, id, group);
  SetWindowTheme(h, L"DarkMode_CFD", nullptr);
  return h;
}

HWND checkbox(const wchar_t* text, int id, std::vector<HWND>& group) {
  HWND h = make(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id, group);
  SetWindowTheme(h, L"", L""); // the classic look takes our text colour
  return h;
}

HWND combo(int id, std::vector<HWND>& group) {
  HWND h = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id, group);
  SetWindowTheme(h, L"DarkMode_CFD", nullptr);
  return h;
}

void makeFonts() {
  for (HFONT* f : {&g.font, &g.bold, &g.big, &g.title, &g.smallFont}) if (*f) DeleteObject(*f);
  auto mk = [](int pt, int weight) {
    return CreateFontW(-MulDiv(pt, int(g.dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe UI");
  };
  g.font = mk(10, FW_NORMAL);
  g.bold = mk(11, FW_SEMIBOLD);
  g.big = mk(30, FW_BOLD);
  g.title = mk(20, FW_BOLD);
  g.smallFont = mk(9, FW_NORMAL);
}

void applyFonts() {
  for (auto* group : {&g.homeCtl, &g.hostCtl, &g.viewCtl})
    for (HWND h : *group) SendMessageW(h, WM_SETFONT, WPARAM(g.font), TRUE);
  for (HWND h : {g.shareHead, g.joinHead, g.viewersHead}) SendMessageW(h, WM_SETFONT, WPARAM(g.bold), TRUE);
  SendMessageW(g.title1, WM_SETFONT, WPARAM(g.title), TRUE);
  SendMessageW(g.roomCode, WM_SETFONT, WPARAM(g.big), TRUE);
  for (HWND h : {g.sub1, g.footer, g.info, g.serverLabel, g.viewInfo}) SendMessageW(h, WM_SETFONT, WPARAM(g.smallFont), TRUE);
}

void createControls() {
  // home
  auto& H = g.homeCtl;
  g.title1 = label(L"Screen Share", H);
  g.sub1 = label(L"4K képernyőmegosztás hanggal, a lehető legkisebb késéssel – saját, titkosított átvitellel.", H);
  g.shareHead = label(L"Képernyő megosztása", H);
  g.passLabel = label(L"Megosztási jelszó", H);
  g.pass = edit(ID_PASSWORD, ES_PASSWORD, H);
  g.remember = checkbox(L"Jelszó megjegyzése", ID_REMEMBER, H);
  g.monLabel = label(L"Képernyő", H);
  g.monitor = combo(ID_MONITOR, H);
  g.qualLabel = label(L"Minőség", H);
  g.quality = combo(ID_QUALITY, H);
  g.audio = checkbox(L"A gép hangja is (5.1 / 7.1 is)", ID_AUDIO, H);
  g.share = button(L"Megosztás indítása", ID_SHARE, H);
  g.shareErr = label(L"", H);
  g.joinHead = label(L"Megosztás nézése", H);
  g.codeLabel = label(L"Szobakód vagy link", H);
  g.code = edit(ID_CODE, 0, H);
  g.join = button(L"Csatlakozás", ID_JOIN, H);
  g.joinErr = label(L"", H);
  g.serverLabel = label(L"Szerver", H);
  g.server = edit(ID_SERVER, 0, H);
  g.footer = label(L"", H);
  g.updateBtn = button(L"", ID_UPDATE, H);
  // sharing
  auto& S = g.hostCtl;
  g.roomLabel = label(L"Szobakód", S);
  g.roomCode = label(L"······", S);
  g.link = edit(0, ES_READONLY, S);
  g.copy = button(L"Link másolása", ID_COPY, S);
  g.status = label(L"", S);
  g.viewersHead = label(L"Nézők", S);
  g.viewers = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, 0, S);
  SetWindowTheme(g.viewers, L"DarkMode_Explorer", nullptr);
  g.info = label(L"", S);
  g.pause = button(L"Szünet", ID_PAUSE, S);
  g.stop = button(L"Megosztás vége", ID_STOP, S);
  // watching
  auto& V = g.viewCtl;
  g.bar = label(L"", V);
  g.viewInfo = label(L"", V);
  g.volume = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, ID_VOLUME, V);
  SendMessageW(g.volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
  SendMessageW(g.volume, TBM_SETPOS, TRUE, 100);
  g.full = button(L"Teljes képernyő", ID_FULL, V);
  g.leave = button(L"Kilépés", ID_LEAVE, V);
  g.overlay = make(L"STATIC", L"", SS_CENTER | SS_NOPREFIX, 0, V);
  g.video = make(L"SSVideo", L"", WS_CLIPSIBLINGS, 0, V);

  for (const Quality& q : kQualities) SendMessageW(g.quality, CB_ADDSTRING, 0, LPARAM(q.name));
  SendMessageW(g.quality, CB_SETCURSEL, std::min<DWORD>(loadDword(L"Quality", 0), DWORD(std::size(kQualities) - 1)), 0);
  g.monitors = listMonitors();
  for (const Monitor& m : g.monitors) SendMessageW(g.monitor, CB_ADDSTRING, 0, LPARAM(m.name.c_str()));
  SendMessageW(g.monitor, CB_SETCURSEL, std::min<DWORD>(loadDword(L"Monitor", 0), DWORD(g.monitors.empty() ? 0 : g.monitors.size() - 1)), 0);
  Button_SetCheck(g.audio, loadDword(L"Audio", 1) ? BST_CHECKED : BST_UNCHECKED);
  std::wstring pw = loadPassword();
  setText(g.pass, pw);
  Button_SetCheck(g.remember, pw.empty() ? BST_UNCHECKED : BST_CHECKED);
  setText(g.server, loadString(L"Server", L"https://mozizas.hu"));
  setText(g.footer, L"Screen Share " SS_VERSION L" · natív alkalmazás");
  SendMessageW(g.pass, EM_SETCUEBANNER, TRUE, LPARAM(L"jelszó"));
  SendMessageW(g.code, EM_SETCUEBANNER, TRUE, LPARAM(L"pl. abc123 vagy https://mozizas.hu/#abc123"));
  applyFonts();
}

/* ------------------------------------------------ layout */

void place(HWND h, int x, int y, int w, int hgt) { SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE); }

void layout() {
  RECT rc;
  GetClientRect(g.wnd, &rc);
  int W = rc.right, Hh = rc.bottom;
  if (g.screen == Screen::Home) {
    int m = px(32), gap = px(24);
    int cw = std::max(px(260), (W - 2 * m - gap) / 2);
    int y = m;
    place(g.title1, m, y, W - 2 * m, px(40));
    y += px(44);
    place(g.sub1, m, y, W - 2 * m, px(22));
    y += px(44);
    int x1 = m, x2 = m + cw + gap, y0 = y, row = px(30), lab = px(20);
    // sharing card
    place(g.shareHead, x1, y, cw, px(26));
    y += px(36);
    place(g.passLabel, x1, y, cw, lab);
    y += lab;
    place(g.pass, x1, y, cw, row);
    y += row + px(6);
    place(g.remember, x1, y, cw, px(22));
    y += px(32);
    place(g.monLabel, x1, y, cw, lab);
    y += lab;
    place(g.monitor, x1, y, cw, px(300));
    y += row + px(10);
    place(g.qualLabel, x1, y, cw, lab);
    y += lab;
    place(g.quality, x1, y, cw, px(300));
    y += row + px(10);
    place(g.audio, x1, y, cw, px(22));
    y += px(34);
    place(g.share, x1, y, cw, px(40));
    y += px(46);
    place(g.shareErr, x1, y, cw, px(40));
    // watching card
    y = y0;
    place(g.joinHead, x2, y, cw, px(26));
    y += px(36);
    place(g.codeLabel, x2, y, cw, lab);
    y += lab;
    place(g.code, x2, y, cw, row);
    y += row + px(12);
    place(g.join, x2, y, cw, px(40));
    y += px(46);
    place(g.joinErr, x2, y, cw, px(40));
    y += px(60);
    place(g.serverLabel, x2, y, cw, lab);
    y += lab;
    place(g.server, x2, y, cw, row);
    place(g.footer, m, Hh - px(30), W / 2, px(20));
    place(g.updateBtn, W - m - px(240), Hh - px(40), px(240), px(32));
  } else if (g.screen == Screen::Hosting) {
    int m = px(32), y = m;
    place(g.roomLabel, m, y, px(200), px(20));
    y += px(22);
    place(g.roomCode, m, y, px(320), px(56));
    place(g.pause, W - m - px(320), y + px(8), px(150), px(40));
    place(g.stop, W - m - px(160), y + px(8), px(160), px(40));
    y += px(66);
    place(g.link, m, y, W - 2 * m - px(170), px(30));
    place(g.copy, W - m - px(160), y - px(2), px(160), px(34));
    y += px(44);
    place(g.status, m, y, W - 2 * m, px(22));
    y += px(30);
    place(g.info, m, y, W - 2 * m, px(40));
    y += px(46);
    place(g.viewersHead, m, y, W - 2 * m, px(26));
    y += px(30);
    place(g.viewers, m, y, W - 2 * m, std::max(px(80), Hh - y - m));
  } else {
    int barH = g.fullscreen ? 0 : px(44);
    place(g.bar, 0, 0, W, barH);
    place(g.viewInfo, px(12), px(12), std::max(0, W - px(560)), px(22));
    place(g.volume, W - px(540), px(8), px(140), px(28));
    place(g.full, W - px(390), px(6), px(170), px(32));
    place(g.leave, W - px(210), px(6), px(200), px(32));
    for (HWND h : {g.bar, g.viewInfo, g.volume, g.full, g.leave}) ShowWindow(h, g.fullscreen ? SW_HIDE : SW_SHOW);
    place(g.video, 0, barH, W, std::max(1, Hh - barH));
    place(g.overlay, px(40), barH + (Hh - barH) / 2 - px(40), std::max(0, W - px(80)), px(80));
    if (g.player) g.player->resized();
  }
}

void show(Screen s) {
  g.screen = s;
  for (HWND h : g.homeCtl) ShowWindow(h, s == Screen::Home ? SW_SHOW : SW_HIDE);
  for (HWND h : g.hostCtl) ShowWindow(h, s == Screen::Hosting ? SW_SHOW : SW_HIDE);
  for (HWND h : g.viewCtl) ShowWindow(h, s == Screen::Viewing ? SW_SHOW : SW_HIDE);
  if (s == Screen::Home) ShowWindow(g.updateBtn, g.update.version.empty() ? SW_HIDE : SW_SHOW);
  if (s == Screen::Viewing) {
    ShowWindow(g.video, g.picture ? SW_SHOW : SW_HIDE);
    ShowWindow(g.overlay, g.picture ? SW_HIDE : SW_SHOW);
    SetFocus(g.wnd);
  }
  layout();
  InvalidateRect(g.wnd, nullptr, TRUE);
}

/* ------------------------------------------------ sessions */

std::string serverUrl() {
  std::string s = narrow(getText(g.server).c_str());
  while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
  if (s.empty()) s = "https://mozizas.hu";
  if (s.find("://") == std::string::npos) s = "https://" + s;
  return s;
}

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

void startSharing() {
  std::wstring pw = getText(g.pass);
  if (pw.empty()) {
    setText(g.shareErr, L"Add meg a megosztási jelszót.");
    return;
  }
  int qi = int(SendMessageW(g.quality, CB_GETCURSEL, 0, 0));
  int mi = int(SendMessageW(g.monitor, CB_GETCURSEL, 0, 0));
  if (mi < 0 || mi >= int(g.monitors.size())) {
    setText(g.shareErr, L"Nem találok képernyőt.");
    return;
  }
  bool audio = Button_GetCheck(g.audio) == BST_CHECKED;
  saveDword(L"Quality", DWORD(std::max(0, qi)));
  saveDword(L"Monitor", DWORD(mi));
  saveDword(L"Audio", audio);
  saveString(L"Server", W(serverUrl()));
  savePassword(Button_GetCheck(g.remember) == BST_CHECKED ? pw : L"");
  setText(g.shareErr, L"");
  endSession();
  int session = ++g.session;
  EngineConfig cfg;
  cfg.server = serverUrl();
  cfg.webSocket = openWinHttpWebSocket;
  EngineCallbacks cb = callbacks(session);
  // the pipeline exists before the engine can call it
  HostPipeline** hostRef = new HostPipeline*(nullptr);
  std::shared_ptr<HostPipeline*> host(hostRef);
  cb.streaming = [host](bool on) { if (*host) (*host)->setStreaming(on); };
  cb.keyframe = [host] { if (*host) (*host)->keyframe(); };
  cb.bitrate = [host](double bps) { if (*host) (*host)->setBitrate(bps); };
  g.engine.reset(new Engine(cfg, cb));
  g.host.reset(new HostPipeline(*g.engine));
  *host = g.host.get();
  const Quality& q = kQualities[qi < 0 ? 0 : qi];
  HostPipeline::Options o;
  o.monitor = g.monitors[size_t(mi)].rect;
  o.maxWidth = q.w;
  o.maxHeight = q.h;
  o.fps = q.fps;
  o.audio = audio;
  g.host->start(o);
  g.live = true;
  g.viewerLines.clear();
  setText(g.roomCode, L"······");
  setText(g.link, L"");
  setText(g.status, L"Kapcsolódás a szerverhez…");
  setText(g.viewers, L"Még nincs néző. Küldd el a linket!");
  setText(g.pause, L"Szünet");
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
    setText(g.joinErr, L"Érvénytelen szobakód.");
    return;
  }
  saveString(L"Server", W(serverUrl()));
  setText(g.joinErr, L"");
  endSession();
  int session = ++g.session;
  EngineConfig cfg;
  cfg.server = serverUrl();
  cfg.webSocket = openWinHttpWebSocket;
  g.player.reset(new VideoPlayer(g.video));
  g.audioOut.reset(new AudioPlayer());
  g.audioOut->setVolume(g.muted ? 0.0f : float(SendMessageW(g.volume, TBM_GETPOS, 0, 0)) / 100.0f);
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
  g.overlayText = "Csatlakozás a szobához…";
  setText(g.overlay, L"Csatlakozás a szobához…");
  setText(g.viewInfo, W("Szoba: " + code));
  show(Screen::Viewing);
  engine->join(code);
}

void stopToHome(const std::wstring& message = L"") {
  if (g.fullscreen) SendMessageW(g.wnd, WM_KEYDOWN, VK_ESCAPE, 0);
  endSession();
  show(Screen::Home);
  if (!message.empty()) setText(g.joinErr, message);
}

/* ------------------------------------------------ engine events (UI thread) */

std::wstring viewerLine(const Json& v) {
  std::string state = v["state"].str();
  std::string s = "Néző #" + std::to_string(int(v["n"].num())) + " · ";
  if (state == "connected") {
    static const std::map<std::string, std::string> paths = {{"lan", "helyi hálózat"}, {"internet", "interneten át"}, {"relay", "a szerveren át"}, {"turn", "TURN relén át"}, {"tcp", "a szerveren át (HTTPS)"}};
    auto p = paths.find(v["path"].str());
    s += "közvetlen mód · " + (p == paths.end() ? v["path"].str() : p->second);
    if (v["fps"].isNumber()) s += " · " + std::to_string(int(v["fps"].num())) + " FPS";
    if (v["kbps"].isNumber()) {
      char b[32];
      std::snprintf(b, sizeof b, " · %.1f Mbit/s", v["kbps"].num() / 1000);
      s += b;
    }
    if (v["rtt"].isNumber()) s += " · RTT " + std::to_string(int(std::lround(v["rtt"].num()))) + " ms";
    if (v["lat"].isNumber()) s += " · késés ~" + std::to_string(int(std::lround(v["lat"].num()))) + " ms";
  } else if (state == "unsupported") {
    s += "nem tud nézni: " + v["whyText"].str("");
  } else if (state == "retrying") {
    s += "kapcsolat újra… (" + v["whyText"].str("") + ")";
  } else {
    s += "kapcsolódik…";
  }
  return W(s);
}

void onEngineEvent(const Json& e) {
  const std::string& type = e["type"].str();
  if (type == "status") {
    setText(g.status, W(e["text"].str()));
    g.statusKind = e["kind"].str();
    InvalidateRect(g.status, nullptr, TRUE);
  } else if (type == "hosted") {
    setText(g.roomCode, W(e["code"].str()));
    g.roomLink = e["link"].str();
    setText(g.link, W(g.roomLink));
    if (e["changed"].b()) MessageBoxW(g.wnd, L"A korábbi szoba lejárt, új link készült – küldd el újra a nézőknek.", L"Screen Share", MB_ICONINFORMATION);
  } else if (type == "host-error") {
    std::string c = e["code"].str();
    std::wstring text = c == "auth" ? L"Hibás megosztási jelszó." : c == "rate" ? L"Túl sok próbálkozás, várj egy kicsit." : c == "full" ? L"A szerver megtelt, próbáld később."
                                                                                                                                        : L"Nem érem el a szervert (" + W(c) + L").";
    endSession();
    show(Screen::Home);
    setText(g.shareErr, text);
  } else if (type == "viewers") {
    std::wstring all;
    for (const Json& v : e["list"].items()) all += viewerLine(v) + L"\r\n";
    if (all.empty()) all = L"Még nincs néző. Küldd el a linket!";
    if (all != getText(g.viewers)) setText(g.viewers, all);
  } else if (type == "closed" && g.screen == Screen::Hosting) {
    stopToHome(L"A szobát egy másik ablak vette át.");
  } else if (type == "view-state" && g.screen == Screen::Viewing) {
    bool picture = e["picture"].b();
    if (picture != g.picture) {
      g.picture = picture;
      ShowWindow(g.video, picture ? SW_SHOW : SW_HIDE);
      ShowWindow(g.overlay, picture ? SW_HIDE : SW_SHOW);
    }
    if (!picture) setText(g.overlay, W(e["text"].str()));
  } else if (type == "view-stats") {
    g.viewStats = e;
  } else if (type == "closed" && g.screen == Screen::Viewing) {
    g.picture = false;
    ShowWindow(g.video, SW_HIDE);
    ShowWindow(g.overlay, SW_SHOW);
  }
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
    }
    s += i.audioLayout.empty() ? " · hang nélkül" : " · hang: " + i.audioLayout;
    setText(g.info, W(s));
  } else if (g.screen == Screen::Viewing && g.player) {
    VideoPlayer::Stats st = g.player->stats();
    if (g.engine && st.latencyMs >= 0) g.engine->reportLatency(st.latencyMs, st.fps);
    const Json& v = g.viewStats;
    std::string s;
    if (g.picture && st.width) {
      char b[200];
      std::snprintf(b, sizeof b, "%d×%d · %.0f FPS", st.width, st.height, st.fps);
      s = b;
      if (st.latencyMs >= 0) s += " · " + std::to_string(int(std::lround(st.latencyMs))) + " ms";
      static const std::map<std::string, std::string> paths = {{"lan", "helyi hálózat"}, {"internet", "interneten át"}, {"relay", "a szerveren át"}, {"turn", "TURN relén át"}, {"tcp", "HTTPS"}};
      auto p = paths.find(v["path"].str());
      s += " · közvetlen" + std::string(p == paths.end() ? "" : " (" + p->second + ")");
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
      }
    } else {
      s = "Várakozás a képre…";
    }
    setText(g.viewInfo, W(s));
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
  layout();
}

void setMuted(bool muted) {
  g.muted = muted;
  if (g.audioOut) g.audioOut->setVolume(muted ? 0.0f : float(SendMessageW(g.volume, TBM_GETPOS, 0, 0)) / 100.0f);
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

void installUpdate() {
  if (g.updating || g.update.version.empty()) return;
  g.updating = true;
  setText(g.updateBtn, L"Letöltés…");
  HWND wnd = g.wnd;
  UpdateInfo u = g.update;
  std::thread([wnd, u] {
    bool ok = downloadAndInstall(u, [wnd](int pct) { PostMessageW(wnd, WM_UPDATE_PROGRESS, WPARAM(pct), 0); });
    PostMessageW(wnd, WM_UPDATE_DONE, ok, 0);
  }).detach();
}

/* ------------------------------------------------ painting */

void drawButton(const DRAWITEMSTRUCT* d) {
  int id = int(d->CtlID);
  bool primary = id == ID_SHARE || id == ID_JOIN || id == ID_UPDATE;
  bool danger = id == ID_STOP || id == ID_LEAVE;
  bool pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED;
  COLORREF fill = primary ? kAccent : danger ? RGB(0x3A, 0x22, 0x2A) : RGB(0x2C, 0x2C, 0x3A);
  if (pressed) fill = RGB(GetRValue(fill) * 4 / 5, GetGValue(fill) * 4 / 5, GetBValue(fill) * 4 / 5);
  HBRUSH bgb = CreateSolidBrush(id == ID_FULL || id == ID_LEAVE ? kCard : kBg);
  FillRect(d->hDC, &d->rcItem, bgb);
  DeleteObject(bgb);
  HBRUSH b = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, fill);
  HGDIOBJ ob = SelectObject(d->hDC, b), op = SelectObject(d->hDC, pen);
  RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, px(10), px(10));
  SelectObject(d->hDC, ob);
  SelectObject(d->hDC, op);
  DeleteObject(b);
  DeleteObject(pen);
  wchar_t text[128];
  GetWindowTextW(d->hwndItem, text, 128);
  SetBkMode(d->hDC, TRANSPARENT);
  SetTextColor(d->hDC, disabled ? kMuted : primary ? RGB(255, 255, 255) : kText);
  HGDIOBJ of = SelectObject(d->hDC, primary ? g.bold : g.font);
  RECT r = d->rcItem;
  DrawTextW(d->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(d->hDC, of);
}

void paintBackground(HDC dc) {
  RECT rc;
  GetClientRect(g.wnd, &rc);
  FillRect(dc, &rc, g.bgBrush);
  if (g.screen == Screen::Viewing && !g.fullscreen) {
    RECT bar = {0, 0, rc.right, px(44)};
    FillRect(dc, &bar, g.cardBrush);
  }
  if (g.screen == Screen::Viewing && !g.picture) {
    RECT rest = {0, g.fullscreen ? 0 : px(44), rc.right, rc.bottom};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rest, black);
    DeleteObject(black);
  }
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

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g.wnd = h;
      g.dpi = GetDpiForWindow(h);
      makeFonts();
      createControls();
      show(Screen::Home);
      SetTimer(h, 1, 1000, nullptr);
      checkUpdatesLater();
      return 0;
    case WM_DPICHANGED: {
      g.dpi = HIWORD(wp);
      makeFonts();
      applyFonts();
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      layout();
      return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {px(720), px(560)};
      return 0;
    }
    case WM_ERASEBKGND: paintBackground(HDC(wp)); return 1;
    case WM_CTLCOLORSTATIC: {
      HDC dc = HDC(wp);
      HWND c = HWND(lp);
      bool onBar = c == g.viewInfo;
      SetBkColor(dc, onBar ? kCard : c == g.overlay ? RGB(0, 0, 0) : kBg);
      COLORREF color = kText;
      if (c == g.sub1 || c == g.footer || c == g.info || c == g.roomLabel || c == g.passLabel || c == g.monLabel || c == g.qualLabel || c == g.codeLabel || c == g.serverLabel) color = kMuted;
      if (c == g.shareErr || c == g.joinErr) color = kAccent;
      if (c == g.status) color = g.statusKind == "ok" ? kOk : g.statusKind == "warn" ? kWarn : g.statusKind == "error" ? kAccent : kMuted;
      if (c == g.roomCode) color = kAccent;
      if (c == g.link || c == g.viewers) {
        SetBkColor(dc, kEdit);
        SetTextColor(dc, kText);
        return LRESULT(g.editBrush);
      }
      SetTextColor(dc, color);
      static HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
      return LRESULT(onBar ? g.cardBrush : c == g.overlay ? black : g.bgBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      SetBkColor(HDC(wp), kEdit);
      SetTextColor(HDC(wp), kText);
      return LRESULT(g.editBrush);
    case WM_DRAWITEM: drawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case ID_SHARE: startSharing(); break;
        case ID_JOIN: startWatching(parseCode(getText(g.code))); break;
        case ID_COPY:
          if (!g.roomLink.empty() && OpenClipboard(h)) {
            std::wstring t = W(g.roomLink);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
            std::memcpy(GlobalLock(mem), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            EmptyClipboard();
            SetClipboardData(CF_UNICODETEXT, mem);
            CloseClipboard();
            setText(g.copy, L"Kimásolva ✓");
            SetTimer(h, 2, 2000, nullptr);
          }
          break;
        case ID_PAUSE:
          g.live = !g.live;
          if (g.engine) g.engine->setLive(g.live);
          setText(g.pause, g.live ? L"Szünet" : L"Folytatás");
          break;
        case ID_STOP: stopToHome(); break;
        case ID_FULL: toggleFullscreen(); break;
        case ID_LEAVE: stopToHome(); break;
        case ID_UPDATE: installUpdate(); break;
        case ID_CODE:
          if (HIWORD(wp) == EN_CHANGE) setText(g.joinErr, L"");
          break;
        default: break;
      }
      return 0;
    case WM_HSCROLL:
      if (HWND(lp) == g.volume) setMuted(false);
      return 0;
    case WM_KEYDOWN:
      if (g.screen == Screen::Viewing) {
        if (wp == 'F' || (wp == VK_ESCAPE && g.fullscreen)) toggleFullscreen();
        else if (wp == 'S') g.stats = !g.stats;
        else if (wp == 'M') setMuted(!g.muted);
      }
      return 0;
    case WM_TIMER:
      if (wp == 1) tick();
      if (wp == 2) {
        KillTimer(h, 2);
        setText(g.copy, L"Link másolása");
      }
      return 0;
    case WM_ENGINE: {
      std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
      if (int(wp) == g.session) onEngineEvent(Json::parse(*text));
      return 0;
    }
    case WM_UPDATE: {
      std::unique_ptr<UpdateInfo> u(reinterpret_cast<UpdateInfo*>(lp));
      g.update = *u;
      setText(g.updateBtn, W("Frissítés: " + u->version));
      if (g.screen == Screen::Home) ShowWindow(g.updateBtn, SW_SHOW);
      return 0;
    }
    case WM_UPDATE_PROGRESS:
      setText(g.updateBtn, L"Letöltés… " + std::to_wstring(int(wp)) + L"%");
      return 0;
    case WM_UPDATE_DONE:
      if (wp) {
        DestroyWindow(h); // the installer starts the new version
      } else {
        g.updating = false;
        setText(g.updateBtn, L"Frissítés sikertelen – újra");
      }
      return 0;
    case WM_CLOSE:
      if (g.screen == Screen::Hosting && MessageBoxW(h, L"Biztosan befejezed a megosztást?", L"Screen Share", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
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
  timeBeginPeriod(1); // 1 ms timers: the capture and the network loop wake on time
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  g.inst = inst;
  g.bgBrush = CreateSolidBrush(kBg);
  g.cardBrush = CreateSolidBrush(kCard);
  g.editBrush = CreateSolidBrush(kEdit);
  HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hIcon = icon;
  wc.hIconSm = icon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"ScreenShare";
  RegisterClassExW(&wc);
  WNDCLASSEXW vc{sizeof vc};
  vc.style = CS_DBLCLKS;
  vc.lpfnWndProc = videoProc;
  vc.hInstance = inst;
  vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  vc.lpszClassName = L"SSVideo";
  RegisterClassExW(&vc);
  if (argc == 6 && (std::wstring(argv[1]) == L"--smoke-host" || std::wstring(argv[1]) == L"--smoke-view")) return smoke(argc, argv);
  HWND wnd = CreateWindowExW(0, L"ScreenShare", L"Screen Share", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, nullptr, nullptr, inst, nullptr);
  // dark title bar (Windows 10 2004+ / 11)
  BOOL dark = TRUE;
  typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
  if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
    FARPROC f = GetProcAddress(dwm, "DwmSetWindowAttribute");
    SetAttr set;
    std::memcpy(&set, &f, sizeof set);
    if (set) set(wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
  }
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
    if (msg.message == WM_KEYDOWN && g.screen == Screen::Viewing) {
      // keys in the viewer go to the window, whichever child has the focus
      SendMessageW(g.wnd, WM_KEYDOWN, msg.wParam, msg.lParam);
      continue;
    }
    if (IsDialogMessageW(wnd, &msg)) {
      if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
        HWND f = GetFocus();
        if (f == g.pass) startSharing();
        else if (f == g.code) startWatching(parseCode(getText(g.code)));
      }
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  timeEndPeriod(1);
  MFShutdown();
  CoUninitialize();
  return 0;
}
