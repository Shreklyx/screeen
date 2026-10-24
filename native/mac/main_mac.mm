// Screen Share for macOS – native (AppKit), without Electron or Chromium.
//
// Home: share the screen (sharing password, which display, quality, system sound) or watch a room
// (code or link). Sharing: the room code and link, the viewers with their connection and latency.
// Watching: the picture fills the window (F / double click: full screen, S: statistics, M: mute).
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Security/Security.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <string>

#include "clock.h"
#include "engine.h"
#include "host_mac.h"
#include "player_mac.h"

#ifndef SS_VERSION
#define SS_VERSION "2.0.0"
#endif

namespace ss {
std::unique_ptr<WebSocket> openMacWebSocket(const std::string& url, WebSocketEvents events);
}
using namespace ss;

static NSString* S(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
static std::string U(NSString* s) { return s.UTF8String ?: ""; }

static NSString* const kFeed = @"https://github.com/Shreklyx/screeen/releases/download/desktop-latest";

struct Quality {
  const char* name;
  int w, h, fps;
};
static const Quality kQualities[] = {
    {"Automatikus – a legszebb (4K, 60 FPS)", 3840, 2160, 60},
    {"1440p, 60 FPS", 2560, 1440, 60},
    {"1080p, 60 FPS", 1920, 1080, 60},
    {"1080p, 120 FPS (játék)", 1920, 1080, 120},
    {"720p, 60 FPS (gyenge net)", 1280, 720, 60},
    {"Eredeti felbontás, 60 FPS", 0, 0, 60},
};

/* ------------------------------------------------ the password in the Keychain */

static void savePassword(NSString* pw) {
  NSDictionary* q = @{(__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword, (__bridge id)kSecAttrService : @"ScreenShare", (__bridge id)kSecAttrAccount : @"share"};
  SecItemDelete((__bridge CFDictionaryRef)q);
  if (!pw.length) return;
  NSMutableDictionary* add = [q mutableCopy];
  add[(__bridge id)kSecValueData] = [pw dataUsingEncoding:NSUTF8StringEncoding];
  SecItemAdd((__bridge CFDictionaryRef)add, nullptr);
}

static NSString* loadPassword() {
  NSDictionary* q = @{(__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword, (__bridge id)kSecAttrService : @"ScreenShare", (__bridge id)kSecAttrAccount : @"share",
                      (__bridge id)kSecReturnData : @YES, (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne};
  CFTypeRef out = nullptr;
  if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &out) != errSecSuccess || !out) return @"";
  NSData* d = (__bridge_transfer NSData*)out;
  return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
}

/* ------------------------------------------------ the video view (double click: full screen) */

@interface SSVideoView : NSView
@property(copy) void (^onDoubleClick)(void);
@end
@implementation SSVideoView
- (instancetype)initWithFrame:(NSRect)r {
  if ((self = [super initWithFrame:r])) {
    self.layer = [CALayer layer]; // layer-hosting: the layer first, then wantsLayer
    self.wantsLayer = YES;
    self.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
  }
  return self;
}
- (void)mouseDown:(NSEvent*)e {
  if (e.clickCount == 2 && self.onDoubleClick) self.onDoubleClick();
}
@end

/* ------------------------------------------------ the app */

@interface SSApp : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation SSApp {
  NSWindow* _window;
  NSView *_home, *_host, *_viewer;
  // home
  NSSecureTextField* _password;
  NSButton *_remember, *_audio, *_shareButton, *_joinButton, *_updateButton;
  NSPopUpButton *_display, *_quality;
  NSTextField *_shareError, *_code, *_joinError, *_server, *_footer;
  // sharing
  NSTextField *_roomCode, *_link, *_status, *_info;
  NSTextView* _viewers;
  NSButton *_pause, *_copy;
  // watching
  SSVideoView* _video;
  NSTextField *_viewInfo, *_overlay;
  NSSlider* _volume;
  NSView* _bar;

  std::unique_ptr<Engine> _engine;
  std::unique_ptr<MacHost> _hostPipe;
  std::unique_ptr<MacPlayer> _player;
  std::unique_ptr<MacAudioOut> _audioOut;
  std::vector<MacDisplay> _displays;
  int _session;
  bool _live, _picture, _stats, _muted;
  std::string _roomLink;
  Json _viewStats;
  NSTimer* _timer;
  NSString* _updateFile;
}

static NSTextField* label(NSString* text, CGFloat size, NSFontWeight weight, NSColor* color) {
  NSTextField* l = [NSTextField labelWithString:text];
  l.font = [NSFont systemFontOfSize:size weight:weight];
  l.textColor = color;
  l.lineBreakMode = NSLineBreakByWordWrapping;
  return l;
}

static NSButton* button(NSString* title, id target, SEL action, bool primary) {
  NSButton* b = [NSButton buttonWithTitle:title target:target action:action];
  b.bezelStyle = NSBezelStyleRounded;
  b.controlSize = NSControlSizeLarge;
  if (primary) {
    b.keyEquivalent = @"";
    b.bezelColor = [NSColor colorWithSRGBRed:1 green:0.30 blue:0.37 alpha:1];
  }
  return b;
}

static NSStackView* vstack(NSArray<NSView*>* views, CGFloat spacing) {
  NSStackView* s = [NSStackView stackViewWithViews:views];
  s.orientation = NSUserInterfaceLayoutOrientationVertical;
  s.alignment = NSLayoutAttributeLeading;
  s.spacing = spacing;
  return s;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
  [NSApp setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
  [self makeMenu];
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1040, 720)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"Screen Share";
  _window.minSize = NSMakeSize(760, 560);
  _window.delegate = self;
  _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
  [_window center];
  _live = true;
  [self makeHome];
  [self makeHost];
  [self makeViewer];
  [self show:_home];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  _timer = [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [self checkUpdates];
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                        handler:^NSEvent*(NSEvent* e) {
                                          if (self->_viewer.superview && !(e.modifierFlags & NSEventModifierFlagCommand)) {
                                            NSString* k = e.charactersIgnoringModifiers.lowercaseString;
                                            if ([k isEqualToString:@"f"]) [self->_window toggleFullScreen:nil];
                                            else if ([k isEqualToString:@"s"]) self->_stats = !self->_stats;
                                            else if ([k isEqualToString:@"m"]) [self setMuted:!self->_muted];
                                            else return e;
                                            return nil;
                                          }
                                          return e;
                                        }];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { return YES; }

- (void)applicationWillTerminate:(NSNotification*)n { [self endSession]; }

- (void)makeMenu {
  NSMenu* bar = [NSMenu new];
  NSMenuItem* appItem = [NSMenuItem new];
  [bar addItem:appItem];
  NSMenu* app = [NSMenu new];
  [app addItemWithTitle:@"Frissítések keresése…" action:@selector(checkUpdatesNow) keyEquivalent:@""];
  [app addItem:[NSMenuItem separatorItem]];
  [app addItemWithTitle:@"Kilépés a Screen Share-ből" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = app;
  NSMenuItem* editItem = [NSMenuItem new];
  [bar addItem:editItem];
  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Szerkesztés"];
  [edit addItemWithTitle:@"Kivágás" action:@selector(cut:) keyEquivalent:@"x"];
  [edit addItemWithTitle:@"Másolás" action:@selector(copy:) keyEquivalent:@"c"];
  [edit addItemWithTitle:@"Beillesztés" action:@selector(paste:) keyEquivalent:@"v"];
  [edit addItemWithTitle:@"Az összes kijelölése" action:@selector(selectAll:) keyEquivalent:@"a"];
  editItem.submenu = edit;
  NSMenuItem* viewItem = [NSMenuItem new];
  [bar addItem:viewItem];
  NSMenu* view = [[NSMenu alloc] initWithTitle:@"Nézet"];
  [view addItemWithTitle:@"Teljes képernyő" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  viewItem.submenu = view;
  NSApp.mainMenu = bar;
}

/* ---- the three screens */

- (void)makeHome {
  NSColor* muted = NSColor.secondaryLabelColor;
  NSTextField* title = label(@"Screen Share", 30, NSFontWeightBold, NSColor.labelColor);
  NSTextField* sub = label(@"4K képernyőmegosztás hanggal, a lehető legkisebb késéssel – saját, titkosított átvitellel.", 13, NSFontWeightRegular, muted);
  // sharing
  _password = [NSSecureTextField new];
  _password.placeholderString = @"jelszó";
  _password.stringValue = loadPassword();
  _remember = [NSButton checkboxWithTitle:@"Jelszó megjegyzése" target:nil action:nil];
  _remember.state = _password.stringValue.length ? NSControlStateValueOn : NSControlStateValueOff;
  _display = [NSPopUpButton new];
  _displays = listDisplays();
  for (const MacDisplay& d : _displays) [_display addItemWithTitle:S(d.name)];
  _quality = [NSPopUpButton new];
  for (const Quality& q : kQualities) [_quality addItemWithTitle:@(q.name)];
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  [_quality selectItemAtIndex:std::min<NSInteger>([ud integerForKey:@"quality"], NSInteger(std::size(kQualities)) - 1)];
  _audio = [NSButton checkboxWithTitle:@"A gép hangja is (macOS 13+)" target:nil action:nil];
  _audio.state = [ud objectForKey:@"audio"] && ![ud boolForKey:@"audio"] ? NSControlStateValueOff : NSControlStateValueOn;
  _shareButton = button(@"Megosztás indítása", self, @selector(share), true);
  _shareError = label(@"", 12, NSFontWeightRegular, NSColor.systemRedColor);
  NSStackView* shareCol = vstack(@[
    label(@"Képernyő megosztása", 17, NSFontWeightSemibold, NSColor.labelColor), label(@"Megosztási jelszó", 12, NSFontWeightRegular, muted), _password, _remember,
    label(@"Képernyő", 12, NSFontWeightRegular, muted), _display, label(@"Minőség", 12, NSFontWeightRegular, muted), _quality, _audio, _shareButton, _shareError
  ], 8);
  // watching
  _code = [NSTextField new];
  _code.placeholderString = @"pl. abc123 vagy https://mozizas.hu/#abc123";
  _joinButton = button(@"Csatlakozás", self, @selector(join), true);
  _joinError = label(@"", 12, NSFontWeightRegular, NSColor.systemRedColor);
  _server = [NSTextField new];
  _server.stringValue = [ud stringForKey:@"server"] ?: @"https://mozizas.hu";
  NSStackView* joinCol = vstack(@[
    label(@"Megosztás nézése", 17, NSFontWeightSemibold, NSColor.labelColor), label(@"Szobakód vagy link", 12, NSFontWeightRegular, muted), _code, _joinButton, _joinError,
    label(@"Szerver", 12, NSFontWeightRegular, muted), _server
  ], 8);
  for (NSView* v in @[ _password, _display, _quality, _code, _server ]) [v.widthAnchor constraintEqualToConstant:380].active = YES;
  NSStackView* cols = [NSStackView stackViewWithViews:@[ shareCol, joinCol ]];
  cols.alignment = NSLayoutAttributeTop;
  cols.spacing = 40;
  _footer = label([NSString stringWithFormat:@"Screen Share %s · natív alkalmazás", SS_VERSION], 11, NSFontWeightRegular, muted);
  _updateButton = button(@"", self, @selector(installUpdate), true);
  _updateButton.hidden = YES;
  NSStackView* footer = [NSStackView stackViewWithViews:@[ _footer, _updateButton ]];
  footer.spacing = 16;
  NSStackView* all = vstack(@[ title, sub, cols, footer ], 18);
  [all setCustomSpacing:28 afterView:sub];
  _home = [NSView new];
  all.translatesAutoresizingMaskIntoConstraints = NO;
  [_home addSubview:all];
  [all.topAnchor constraintEqualToAnchor:_home.topAnchor constant:32].active = YES;
  [all.leadingAnchor constraintEqualToAnchor:_home.leadingAnchor constant:32].active = YES;
}

- (void)makeHost {
  NSColor* muted = NSColor.secondaryLabelColor;
  _roomCode = label(@"······", 44, NSFontWeightBold, [NSColor colorWithSRGBRed:1 green:0.30 blue:0.37 alpha:1]);
  _roomCode.selectable = YES;
  _link = [NSTextField new];
  _link.editable = NO;
  [_link.widthAnchor constraintEqualToConstant:520].active = YES;
  _copy = button(@"Link másolása", self, @selector(copyLink), false);
  NSStackView* linkRow = [NSStackView stackViewWithViews:@[ _link, _copy ]];
  _status = label(@"", 13, NSFontWeightRegular, muted);
  _info = label(@"", 12, NSFontWeightRegular, muted);
  NSScrollView* scroll = [NSTextView scrollableTextView];
  _viewers = scroll.documentView;
  _viewers.editable = NO;
  _viewers.font = [NSFont systemFontOfSize:13];
  _viewers.textContainerInset = NSMakeSize(6, 6);
  [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
  [scroll.widthAnchor constraintEqualToConstant:700].active = YES;
  _pause = button(@"Szünet", self, @selector(togglePause), false);
  NSButton* stop = button(@"Megosztás vége", self, @selector(stopToHome), false);
  NSStackView* buttons = [NSStackView stackViewWithViews:@[ _pause, stop ]];
  NSStackView* all = vstack(@[ label(@"Szobakód", 12, NSFontWeightRegular, muted), _roomCode, linkRow, _status, _info, label(@"Nézők", 16, NSFontWeightSemibold, NSColor.labelColor), scroll, buttons ], 10);
  _host = [NSView new];
  all.translatesAutoresizingMaskIntoConstraints = NO;
  [_host addSubview:all];
  [all.topAnchor constraintEqualToAnchor:_host.topAnchor constant:28].active = YES;
  [all.leadingAnchor constraintEqualToAnchor:_host.leadingAnchor constant:32].active = YES;
  [all.bottomAnchor constraintLessThanOrEqualToAnchor:_host.bottomAnchor constant:-24].active = YES;
}

- (void)makeViewer {
  _viewer = [NSView new];
  _bar = [NSView new];
  _bar.wantsLayer = YES;
  _bar.layer.backgroundColor = [NSColor colorWithSRGBRed:0.11 green:0.11 blue:0.16 alpha:1].CGColor;
  _viewInfo = label(@"", 12, NSFontWeightRegular, NSColor.labelColor);
  _volume = [NSSlider sliderWithValue:100 minValue:0 maxValue:100 target:self action:@selector(volumeChanged)];
  [_volume.widthAnchor constraintEqualToConstant:120].active = YES;
  NSButton* full = button(@"Teljes képernyő", self, @selector(fullScreen), false);
  NSButton* leave = button(@"Kilépés", self, @selector(stopToHome), false);
  [_viewInfo setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  NSStackView* barStack = [NSStackView stackViewWithViews:@[ _viewInfo, _volume, full, leave ]];
  barStack.translatesAutoresizingMaskIntoConstraints = NO;
  [_bar addSubview:barStack];
  _video = [SSVideoView new];
  __weak SSApp* weak = self;
  _video.onDoubleClick = ^{ [weak fullScreen]; };
  _overlay = label(@"", 16, NSFontWeightMedium, NSColor.whiteColor);
  _overlay.alignment = NSTextAlignmentCenter;
  for (NSView* v in @[ _bar, _video, _overlay ]) {
    v.translatesAutoresizingMaskIntoConstraints = NO;
    [_viewer addSubview:v];
  }
  [NSLayoutConstraint activateConstraints:@[
    [_bar.topAnchor constraintEqualToAnchor:_viewer.topAnchor], [_bar.leadingAnchor constraintEqualToAnchor:_viewer.leadingAnchor],
    [_bar.trailingAnchor constraintEqualToAnchor:_viewer.trailingAnchor], [_bar.heightAnchor constraintEqualToConstant:44],
    [barStack.leadingAnchor constraintEqualToAnchor:_bar.leadingAnchor constant:12], [barStack.trailingAnchor constraintEqualToAnchor:_bar.trailingAnchor constant:-12],
    [barStack.centerYAnchor constraintEqualToAnchor:_bar.centerYAnchor],
    [_video.topAnchor constraintEqualToAnchor:_bar.bottomAnchor], [_video.leadingAnchor constraintEqualToAnchor:_viewer.leadingAnchor],
    [_video.trailingAnchor constraintEqualToAnchor:_viewer.trailingAnchor], [_video.bottomAnchor constraintEqualToAnchor:_viewer.bottomAnchor],
    [_overlay.centerXAnchor constraintEqualToAnchor:_video.centerXAnchor], [_overlay.centerYAnchor constraintEqualToAnchor:_video.centerYAnchor],
    [_overlay.widthAnchor constraintLessThanOrEqualToAnchor:_video.widthAnchor constant:-80],
  ]];
}

- (void)show:(NSView*)v {
  v.frame = _window.contentView.bounds;
  _window.contentView = v;
  if (v == _viewer) [_window makeFirstResponder:_video];
}

/* ---- sessions */

- (std::string)serverUrl {
  std::string s = U([_server.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]);
  while (!s.empty() && s.back() == '/') s.pop_back();
  if (s.empty()) s = "https://mozizas.hu";
  if (s.find("://") == std::string::npos) s = "https://" + s;
  return s;
}

- (EngineCallbacks)callbacks:(int)session {
  EngineCallbacks cb;
  __weak SSApp* weak = self;
  cb.event = [weak, session](const Json& e) {
    std::string text = e.dump();
    dispatch_async(dispatch_get_main_queue(), ^{
      [weak event:text session:session];
    });
  };
  return cb;
}

- (void)event:(const std::string&)text session:(int)session {
  if (session == _session) [self onEvent:Json::parse(text)];
}

- (void)endSession {
  _session++;
  if (_player) _player->detach(); // its decoder thread calls the engine no more
  if (_hostPipe) _hostPipe->stop();
  _engine.reset();
  _hostPipe.reset();
  _player.reset();
  _audioOut.reset();
  _picture = false;
}

- (void)share {
  NSString* pw = _password.stringValue;
  if (!pw.length) {
    _shareError.stringValue = @"Add meg a megosztási jelszót.";
    return;
  }
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  NSInteger qi = std::max<NSInteger>(0, _quality.indexOfSelectedItem);
  [ud setInteger:qi forKey:@"quality"];
  [ud setBool:_audio.state == NSControlStateValueOn forKey:@"audio"];
  [ud setObject:S([self serverUrl]) forKey:@"server"];
  savePassword(_remember.state == NSControlStateValueOn ? pw : @"");
  _shareError.stringValue = @"";
  [self endSession];
  int session = ++_session;
  EngineConfig cfg;
  cfg.server = [self serverUrl];
  cfg.webSocket = openMacWebSocket;
  cfg.encoderName = "VideoToolbox";
  EngineCallbacks cb = [self callbacks:session];
  auto host = std::make_shared<MacHost*>(nullptr);
  cb.streaming = [host](bool on) { if (*host) (*host)->setStreaming(on); };
  cb.keyframe = [host] { if (*host) (*host)->keyframe(); };
  cb.bitrate = [host](double bps) { if (*host) (*host)->setBitrate(bps); };
  _engine.reset(new Engine(cfg, cb));
  _hostPipe.reset(new MacHost(*_engine));
  *host = _hostPipe.get();
  const Quality& q = kQualities[qi];
  MacHost::Options o;
  NSInteger di = _display.indexOfSelectedItem;
  if (di >= 0 && di < NSInteger(_displays.size())) o.display = _displays[size_t(di)].id;
  o.maxWidth = q.w;
  o.maxHeight = q.h;
  o.fps = q.fps;
  o.audio = _audio.state == NSControlStateValueOn;
  _hostPipe->start(o);
  _live = true;
  _roomCode.stringValue = @"······";
  _link.stringValue = @"";
  _status.stringValue = @"Kapcsolódás a szerverhez…";
  _viewers.string = @"Még nincs néző. Küldd el a linket!";
  _pause.title = @"Szünet";
  [self show:_host];
  _engine->host(U(pw));
}

static std::string parseCode(NSString* text) {
  std::string s = U(text);
  size_t hash = s.find('#');
  if (hash != std::string::npos) s = s.substr(hash + 1);
  std::string out;
  for (char c : s)
    if (isalnum(static_cast<unsigned char>(c))) out += char(tolower(c));
  return out;
}

- (void)join {
  [self watch:parseCode(_code.stringValue)];
}

- (void)watch:(const std::string&)code {
  if (code.size() < 4) {
    _joinError.stringValue = @"Érvénytelen szobakód.";
    return;
  }
  [NSUserDefaults.standardUserDefaults setObject:S([self serverUrl]) forKey:@"server"];
  _joinError.stringValue = @"";
  [self endSession];
  int session = ++_session;
  EngineConfig cfg;
  cfg.server = [self serverUrl];
  cfg.webSocket = openMacWebSocket;
  _player.reset(new MacPlayer(_video.layer));
  _audioOut.reset(new MacAudioOut());
  _audioOut->setVolume(_muted ? 0.0f : float(_volume.doubleValue / 100));
  EngineCallbacks cb = [self callbacks:session];
  MacPlayer* player = _player.get();
  MacAudioOut* audio = _audioOut.get();
  cb.media = [player, audio](const MediaMeta& m, const uint8_t* d, size_t n) {
    if (m.stream == 0) player->push(d, n, m.key, m.capture);
    else audio->push(m.sub, d, n, m.ts);
  };
  _engine.reset(new Engine(cfg, cb));
  Engine* engine = _engine.get();
  player->clockOffset = [engine] { return engine->clockOffset(); };
  player->needKeyframe = [engine] { engine->requestKeyframe(); };
  _picture = false;
  _overlay.stringValue = @"Csatlakozás a szobához…";
  _overlay.hidden = NO;
  _viewInfo.stringValue = S("Szoba: " + code);
  [self show:_viewer];
  engine->join(code);
}

- (void)stopToHome {
  if (_window.styleMask & NSWindowStyleMaskFullScreen) [_window toggleFullScreen:nil];
  [self endSession];
  [self show:_home];
}

- (void)togglePause {
  _live = !_live;
  if (_engine) _engine->setLive(_live);
  _pause.title = _live ? @"Szünet" : @"Folytatás";
}

- (void)copyLink {
  if (_roomLink.empty()) return;
  [NSPasteboard.generalPasteboard clearContents];
  [NSPasteboard.generalPasteboard setString:S(_roomLink) forType:NSPasteboardTypeString];
  _copy.title = @"Kimásolva ✓";
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ self->_copy.title = @"Link másolása"; });
}

- (void)fullScreen {
  [_window toggleFullScreen:nil];
}

- (void)volumeChanged {
  [self setMuted:false];
}

- (void)setMuted:(bool)m {
  _muted = m;
  if (_audioOut) _audioOut->setVolume(m ? 0.0f : float(_volume.doubleValue / 100));
}

/* ---- engine events (main thread) */

static std::string viewerLine(const Json& v) {
  static const std::map<std::string, std::string> paths = {{"lan", "helyi hálózat"}, {"internet", "interneten át"}, {"relay", "a szerveren át"}, {"turn", "TURN relén át"}, {"tcp", "a szerveren át (HTTPS)"}};
  std::string state = v["state"].str();
  std::string s = "Néző #" + std::to_string(int(v["n"].num())) + " · ";
  if (state == "connected") {
    auto p = paths.find(v["path"].str());
    s += "közvetlen mód · " + (p == paths.end() ? v["path"].str() : p->second);
    char b[64];
    if (v["fps"].isNumber()) s += " · " + std::to_string(int(v["fps"].num())) + " FPS";
    if (v["kbps"].isNumber()) {
      snprintf(b, sizeof b, " · %.1f Mbit/s", v["kbps"].num() / 1000);
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
  return s;
}

- (void)onEvent:(const Json&)e {
  const std::string& type = e["type"].str();
  if (type == "status") {
    _status.stringValue = S(e["text"].str());
    std::string k = e["kind"].str();
    _status.textColor = k == "ok" ? NSColor.systemGreenColor : k == "warn" ? NSColor.systemOrangeColor : k == "error" ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
  } else if (type == "hosted") {
    _roomCode.stringValue = S(e["code"].str());
    _roomLink = e["link"].str();
    _link.stringValue = S(_roomLink);
  } else if (type == "host-error") {
    std::string c = e["code"].str();
    NSString* text = c == "auth" ? @"Hibás megosztási jelszó." : c == "rate" ? @"Túl sok próbálkozás, várj egy kicsit." : c == "full" ? @"A szerver megtelt, próbáld később."
                                                                                                                                      : S("Nem érem el a szervert (" + c + ").");
    [self endSession];
    [self show:_home];
    _shareError.stringValue = text;
  } else if (type == "viewers") {
    std::string all;
    for (const Json& v : e["list"].items()) all += viewerLine(v) + "\n";
    _viewers.string = all.empty() ? @"Még nincs néző. Küldd el a linket!" : S(all);
  } else if (type == "closed" && _host.superview) {
    [self stopToHome];
    _joinError.stringValue = @"A szobát egy másik ablak vette át.";
  } else if (type == "view-state" && _viewer.superview) {
    _picture = e["picture"].b();
    _overlay.hidden = _picture;
    if (!_picture) _overlay.stringValue = S(e["text"].str());
  } else if (type == "view-stats") {
    _viewStats = e;
  }
}

- (void)tick {
  if (_host.superview && _hostPipe) {
    MacHost::Info i = _hostPipe->info();
    std::string s;
    char b[200];
    if (!i.error.empty()) {
      s = i.error == "permission" ? "A macOS nem engedi a képernyőfelvételt: Rendszerbeállítások → Adatvédelem és biztonság → Képernyő- és rendszerhangfelvétel → Screen Share, majd indítsd újra."
                                  : "A kép nem indult el: " + i.error;
    } else if (i.width) {
      snprintf(b, sizeof b, "Kép: %d×%d · %s%s", i.width, i.height, i.encoder.c_str(), i.hardware ? " (hardveres)" : "");
      s = b;
      if (i.fps > 0) {
        snprintf(b, sizeof b, " · %.0f FPS · %.1f ms/kép", i.fps, i.encodeMs);
        s += b;
      }
    }
    s += i.audioLayout.empty() ? " · hang nélkül" : " · hang: " + i.audioLayout;
    _info.stringValue = S(s);
  } else if (_viewer.superview && _player) {
    MacPlayer::Stats st = _player->stats();
    if (_engine && st.latencyMs >= 0) _engine->reportLatency(st.latencyMs, st.fps);
    std::string s;
    if (_picture && st.width) {
      char b[200];
      snprintf(b, sizeof b, "%d×%d · %.0f FPS", st.width, st.height, st.fps);
      s = b;
      if (st.latencyMs >= 0) s += " · " + std::to_string(int(std::lround(st.latencyMs))) + " ms";
      s += " · közvetlen";
      const Json& v = _viewStats;
      if (_stats) {
        if (v["kbps"].isNumber()) {
          snprintf(b, sizeof b, " · %.1f Mbit/s", v["kbps"].num() / 1000);
          s += b;
        }
        if (v["rtt"].isNumber()) s += " · RTT " + std::to_string(int(std::lround(v["rtt"].num()))) + " ms";
        if (v["encoder"].isString()) s += " · küldő: " + v["encoder"].str();
        s += std::string(" · dekódolás: ") + (st.hardware ? "hardveres" : "szoftveres");
      }
    } else {
      s = "Várakozás a képre…";
    }
    _viewInfo.stringValue = S(s);
  }
}

/* ---- updates: native-latest.json → the zip → the app replaced, started again */

- (void)checkUpdates {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self checkUpdatesNow]; });
}

- (void)checkUpdatesNow {
  NSURL* url = [NSURL URLWithString:[kFeed stringByAppendingString:@"/native-latest.json"]];
  NSURLRequest* req = [NSURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:20];
  [[NSURLSession.sharedSession dataTaskWithRequest:req
                                 completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                                   if (!data || error) return;
                                   Json j = Json::parse(std::string(static_cast<const char*>(data.bytes), data.length));
                                   std::string v = j["version"].str(""), file = j["mac"].str("");
                                   if (v.empty() || file.empty()) return;
                                   NSString* version = S(v);
                                   if ([version compare:@SS_VERSION options:NSNumericSearch] != NSOrderedDescending) return;
                                   dispatch_async(dispatch_get_main_queue(), ^{
                                     self->_updateFile = S(file);
                                     self->_updateButton.title = [NSString stringWithFormat:@"Frissítés: %@", version];
                                     self->_updateButton.hidden = NO;
                                   });
                                 }] resume];
}

- (void)installUpdate {
  NSString* file = _updateFile;
  if (!file) return;
  _updateButton.title = @"Letöltés…";
  _updateButton.enabled = NO;
  NSURL* url = [NSURL URLWithString:[NSString stringWithFormat:@"%@/%@", kFeed, file]];
  [[NSURLSession.sharedSession downloadTaskWithURL:url
                                  completionHandler:^(NSURL* location, NSURLResponse* response, NSError* error) {
                                    NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"ScreenShareUpdate"];
                                    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
                                    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
                                    NSString* zip = [dir stringByAppendingPathComponent:@"update.zip"];
                                    bool ok = location && !error && [NSFileManager.defaultManager moveItemAtPath:location.path toPath:zip error:nil];
                                    if (ok) {
                                      NSTask* unzip = [NSTask launchedTaskWithLaunchPath:@"/usr/bin/ditto" arguments:@[ @"-x", @"-k", zip, dir ]];
                                      [unzip waitUntilExit];
                                      ok = unzip.terminationStatus == 0;
                                    }
                                    NSString* fresh = [dir stringByAppendingPathComponent:@"Screen Share.app"];
                                    ok = ok && [NSFileManager.defaultManager fileExistsAtPath:fresh];
                                    dispatch_async(dispatch_get_main_queue(), ^{
                                      if (!ok) {
                                        self->_updateButton.title = @"Frissítés sikertelen – újra";
                                        self->_updateButton.enabled = YES;
                                        return;
                                      }
                                      // after we quit: the new app in place of this one, then start it
                                      NSString* target = NSBundle.mainBundle.bundlePath;
                                      NSString* script = [NSString stringWithFormat:@"while kill -0 %d 2>/dev/null; do sleep 0.2; done; rm -rf \"%@\" && mv \"%@\" \"%@\" && xattr -dr com.apple.quarantine \"%@\" 2>/dev/null; open \"%@\"",
                                                                                   NSProcessInfo.processInfo.processIdentifier, target, fresh, target, target, target];
                                      [NSTask launchedTaskWithLaunchPath:@"/bin/sh" arguments:@[ @"-c", script ]];
                                      [NSApp terminate:nil];
                                    });
                                  }] resume];
}

/* ---- window */

- (BOOL)windowShouldClose:(NSWindow*)w {
  if (!_host.superview) return YES;
  NSAlert* a = [NSAlert new];
  a.messageText = @"Biztosan befejezed a megosztást?";
  [a addButtonWithTitle:@"Befejezés"];
  [a addButtonWithTitle:@"Mégse"];
  return [a runModal] == NSAlertFirstButtonReturn;
}

@end

/* ------------------------------------------------ smoke tests (CI): the real pipeline, no UI */

static int smoke(NSArray<NSString*>* args) {
  std::string mode = U(args[1]), server = U(args[2]), secret = U(args[3]);
  int seconds = args[4].intValue;
  FILE* log = fopen(args[5].fileSystemRepresentation, "w");
  auto line = [log](const std::string& s) {
    fprintf(log, "%s\n", s.c_str());
    fflush(log);
  };
  EngineConfig cfg;
  cfg.server = server;
  cfg.webSocket = openMacWebSocket;
  EngineCallbacks cb;
  cb.event = [&](const Json& e) {
    if (e["type"].str() == "hosted") line("CODE " + e["code"].str());
    if (e["type"].str() != "viewers") line("EVENT " + e.dump());
  };
  Json result = Json::object();
  auto runFor = [&](double s) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:s];
    while ([until timeIntervalSinceNow] > 0) [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
  };
  if (mode == "--smoke-host") {
    MacHost* hostPtr = nullptr;
    cb.streaming = [&](bool on) { if (hostPtr) hostPtr->setStreaming(on); };
    cb.keyframe = [&] { if (hostPtr) hostPtr->keyframe(); };
    cb.bitrate = [&](double bps) { if (hostPtr) hostPtr->setBitrate(bps); };
    Engine engine(cfg, cb);
    MacHost host(engine);
    hostPtr = &host;
    MacHost::Options o;
    o.fps = 30;
    host.start(o);
    engine.host(secret);
    runFor(seconds);
    MacHost::Info i = host.info();
    result.set("encoder", i.encoder).set("width", i.width).set("height", i.height).set("fps", i.fps).set("error", i.error).set("audio", i.audioLayout);
    result.set("audioBuffers", i.audioBuffers).set("audioPackets", i.audioPackets);
    host.stop();
    hostPtr = nullptr;
  } else {
    CALayer* layer = [CALayer layer];
    MacPlayer player(layer);
    MacAudioOut audio;
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
    MacPlayer::Stats st;
    for (int t = 0; t < seconds * 10; t++) {
      runFor(0.1);
      st = player.stats();
      best = std::max(best, st.fps);
    }
    player.detach();
    result.set("received", frames.load()).set("audioPackets", sounds.load()).set("shownFps", best).set("width", st.width).set("height", st.height).set("latencyMs", st.latencyMs);
  }
  line("RESULT " + result.dump());
  fclose(log);
  return 0;
}

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    netInit();
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    if (args.count == 6 && ([args[1] isEqualToString:@"--smoke-host"] || [args[1] isEqualToString:@"--smoke-view"])) return smoke(args);
    NSApplication* app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    SSApp* delegate = [SSApp new];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
