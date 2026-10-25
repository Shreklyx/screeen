// Screen Share for macOS – native (AppKit), without Electron or Chromium.
//
// A Mac app like the system's own: its standard controls (Liquid Glass on macOS 26), settings in
// rounded groups as in System Settings, questions in sheets, the standard About panel.
// Home: share the screen (password, display, quality, system sound) or watch a room (code or
// link). Sharing: the room code and link, the viewers with their connection and latency.
// Watching: the window grows, the picture fills it, a floating glass bar over it that fades away
// (F / double click: full screen, S: statistics, M: mute). The server is always mozizas.hu.
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
#include <vector>

#include "clock.h"
#include "engine.h"
#include "host_mac.h"
#include "player_mac.h"

#ifndef SS_VERSION
#define SS_VERSION "2.0.0"
#endif
#ifndef SS_BUILD
#define SS_BUILD "0"
#endif

namespace ss {
std::unique_ptr<WebSocket> openMacWebSocket(const std::string& url, WebSocketEvents events);
}
using namespace ss;

static NSString* S(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
static std::string U(NSString* s) { return s.UTF8String ?: ""; }

static const char* const kServer = "https://mozizas.hu";
static NSString* const kFeed = @"https://github.com/Shreklyx/screeen/releases/download/desktop-latest";
static NSString* const kSite = @"https://mozizas.hu";

// the compact window: content width, margins (the title bar is part of the window's content)
static const CGFloat kHomeWidth = 420, kHostWidth = 440, kSide = 30, kTop = 46, kBottom = 26;

struct Quality {
  const char* name;
  const char* hint;
  int w, h, fps;
};
static const Quality kQualities[] = {
    {"Automatikus", "A legszebb, amit a gép és a hálózat bír: akár 4K, 60 FPS.", 3840, 2160, 60},
    {"1440p · 60 FPS", "2560×1440, 60 FPS.", 2560, 1440, 60},
    {"1080p · 60 FPS", "1920×1080, 60 FPS.", 1920, 1080, 60},
    {"1080p · 120 FPS", "1920×1080, 120 FPS – játékhoz, gyors mozgáshoz.", 1920, 1080, 120},
    {"720p · 60 FPS", "1280×720, 60 FPS – gyenge internetre.", 1280, 720, 60},
    {"Eredeti", "A képernyő saját felbontása, 60 FPS.", 0, 0, 60},
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

/* ------------------------------------------------ this Mac's own signature (sign_local.sh) */

// macOS keeps what was allowed (screen and sound recording, the keychain, the local network) for the
// app's signing identity. The builds are signed ad hoc – another identity for every version – so the
// app signs itself, and each update, with an identity of this Mac's own: asked once, remembered.
static NSString* const kSigningName = @"Screen Share Local Signing";

static bool signedLocally() {
  SecStaticCodeRef code = nullptr;
  if (SecStaticCodeCreateWithPath((__bridge CFURLRef)NSBundle.mainBundle.bundleURL, kSecCSDefaultFlags, &code) != errSecSuccess || !code) return false;
  bool mine = false;
  CFDictionaryRef info = nullptr;
  if (SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info) == errSecSuccess && info) {
    NSArray* certs = ((__bridge NSDictionary*)info)[(__bridge id)kSecCodeInfoCertificates];
    CFStringRef name = nullptr;
    if (certs.count && SecCertificateCopyCommonName((__bridge SecCertificateRef)certs[0], &name) == errSecSuccess && name) {
      mine = [(__bridge NSString*)name isEqualToString:kSigningName];
      CFRelease(name);
    }
    CFRelease(info);
  }
  CFRelease(code);
  return mine;
}

/** The helper, copied out of the app first: the app it is in gets replaced while it runs. */
static NSString* signingHelper() {
  NSString* src = [NSBundle.mainBundle pathForResource:@"sign_local" ofType:@"sh"];
  NSString* dst = [NSTemporaryDirectory() stringByAppendingPathComponent:@"screenshare-sign.sh"];
  [NSFileManager.defaultManager removeItemAtPath:dst error:nil];
  return src && [NSFileManager.defaultManager copyItemAtPath:src toPath:dst error:nil] ? dst : nil;
}

/** Once the app has quit: `fresh` (an update; nil: a signed copy of this one) in its place, signed with
 *  this Mac's identity; then started again. */
static bool swapAfterQuit(NSString* fresh) {
  NSString* helper = signingHelper();
  if (!helper) return false;
  NSString* pid = [NSString stringWithFormat:@"%d", NSProcessInfo.processInfo.processIdentifier];
  [NSTask launchedTaskWithLaunchPath:@"/bin/bash" arguments:@[ helper, @"swap", pid, NSBundle.mainBundle.bundlePath, fresh ?: @"-", @"1" ]];
  return true;
}

/* ------------------------------------------------ small helpers */

static NSImage* symbol(NSString* name, CGFloat size, NSFontWeight weight = NSFontWeightRegular) {
  NSImage* img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
  if (!img) return [NSImage new];
  return [img imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:size weight:weight]] ?: img;
}

/** A label; with `wrapWidth` it wraps at that width. */
static NSTextField* label(NSString* text, CGFloat size, NSFontWeight weight, NSColor* color, CGFloat wrapWidth = 0) {
  NSTextField* l = wrapWidth > 0 ? [NSTextField wrappingLabelWithString:text] : [NSTextField labelWithString:text];
  l.font = [NSFont systemFontOfSize:size weight:weight];
  l.textColor = color;
  l.translatesAutoresizingMaskIntoConstraints = NO;
  if (wrapWidth > 0) {
    l.preferredMaxLayoutWidth = wrapWidth;
    l.selectable = NO;
  } else {
    l.lineBreakMode = NSLineBreakByTruncatingTail;
  }
  return l;
}

static void pin(NSView* v, NSView* to) {
  v.translatesAutoresizingMaskIntoConstraints = NO;
  [NSLayoutConstraint activateConstraints:@[
    [v.leadingAnchor constraintEqualToAnchor:to.leadingAnchor], [v.trailingAnchor constraintEqualToAnchor:to.trailingAnchor],
    [v.topAnchor constraintEqualToAnchor:to.topAnchor], [v.bottomAnchor constraintEqualToAnchor:to.bottomAnchor]
  ]];
}

static NSStackView* vstack(NSArray<NSView*>* views, CGFloat spacing) {
  NSStackView* s = [NSStackView stackViewWithViews:views];
  s.orientation = NSUserInterfaceLayoutOrientationVertical;
  s.alignment = NSLayoutAttributeCenterX;
  s.spacing = spacing;
  s.translatesAutoresizingMaskIntoConstraints = NO;
  return s;
}

static NSStackView* hstack(NSArray<NSView*>* views, CGFloat spacing) {
  NSStackView* s = [NSStackView stackViewWithViews:views];
  s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  s.alignment = NSLayoutAttributeCenterY;
  s.spacing = spacing;
  s.translatesAutoresizingMaskIntoConstraints = NO;
  return s;
}

/** The child as wide as the (vertical) stack, less `inset` on each side. */
static void stretch(NSView* child, NSStackView* stack, CGFloat inset = 0) {
  [child.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-2 * inset].active = YES;
}

static NSView* spacer() {
  NSView* v = [NSView new];
  v.translatesAutoresizingMaskIntoConstraints = NO;
  [v setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
  [v setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
  return v;
}

static void sizeTo(NSView* v, CGFloat w, CGFloat h) {
  v.translatesAutoresizingMaskIntoConstraints = NO;
  if (w > 0) [v.widthAnchor constraintEqualToConstant:w].active = YES;
  if (h > 0) [v.heightAnchor constraintEqualToConstant:h].active = YES;
}

static void animateAlpha(NSView* v, CGFloat to, double seconds = 0.22) {
  [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
    ctx.duration = seconds;
    ctx.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
    v.animator.alphaValue = to;
  }];
}

static void fade(NSView* v, double seconds = 0.22) {
  CATransition* t = [CATransition animation];
  t.type = kCATransitionFade;
  t.duration = seconds;
  t.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
  [v.layer addAnimation:t forKey:@"fade"];
}

static NSView* makeDot(NSColor* color, CGFloat d = 8) {
  NSView* v = [NSView new];
  v.wantsLayer = YES;
  v.layer.backgroundColor = color.CGColor;
  v.layer.cornerRadius = d / 2;
  sizeTo(v, d, d);
  return v;
}

static void setDotColor(NSView* dot, NSColor* color) {
  dot.layer.backgroundColor = color.CGColor;
}

static NSSwitch* makeSwitch(BOOL on) {
  NSSwitch* s = [NSSwitch new];
  s.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  s.controlSize = NSControlSizeSmall;
  s.translatesAutoresizingMaskIntoConstraints = NO;
  return s;
}

static NSButton* button(NSString* title, NSString* sym, id target, SEL action) {
  NSButton* b = sym ? [NSButton buttonWithTitle:title image:symbol(sym, 13) target:target action:action] : [NSButton buttonWithTitle:title target:target action:action];
  if (sym) b.imagePosition = NSImageLeading;
  b.translatesAutoresizingMaskIntoConstraints = NO;
  return b;
}

/** A button with only a symbol, no bezel (on the viewer's bar). */
static NSButton* iconButton(NSString* sym, id target, SEL action) {
  NSButton* b = [NSButton buttonWithImage:symbol(sym, 15) target:target action:action];
  b.bordered = NO;
  sizeTo(b, 30, 28);
  return b;
}

/** One row of a settings group: the title on the left (and a line under it), the control right. */
static NSStackView* formRow(NSString* title, NSString* sub, NSView* control) {
  NSTextField* t = label(title, 13, NSFontWeightRegular, NSColor.labelColor);
  NSView* text = t;
  if (sub) {
    NSStackView* two = vstack(@[ t, label(sub, 11, NSFontWeightRegular, NSColor.secondaryLabelColor) ], 1);
    two.alignment = NSLayoutAttributeLeading;
    text = two;
  }
  control.translatesAutoresizingMaskIntoConstraints = NO;
  NSStackView* r = hstack(@[ text, spacer(), control ], 12);
  r.edgeInsets = NSEdgeInsetsMake(7, 12, 7, 12);
  [r.heightAnchor constraintGreaterThanOrEqualToConstant:40].active = YES;
  return r;
}

/** The glass under the viewer's controls: Liquid Glass on macOS 26, the HUD blur before. */
static NSView* glassCapsule(NSView* content, CGFloat radius) {
  Class glass = NSClassFromString(@"NSGlassEffectView");
  if (glass) {
    NSView* g = [glass new];
    @try {
      [g setValue:@(radius) forKey:@"cornerRadius"];
      [g setValue:content forKey:@"contentView"];
      g.translatesAutoresizingMaskIntoConstraints = NO;
      pin(content, g);
      return g;
    } @catch (NSException*) {
    }
  }
  NSVisualEffectView* v = [NSVisualEffectView new];
  v.material = NSVisualEffectMaterialHUDWindow;
  v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
  v.state = NSVisualEffectStateActive;
  v.wantsLayer = YES;
  v.layer.cornerRadius = radius;
  v.layer.masksToBounds = YES;
  v.translatesAutoresizingMaskIntoConstraints = NO;
  [v addSubview:content];
  pin(content, v);
  return v;
}

/* ------------------------------------------------ a settings group (as in System Settings) */

@interface SSGroup : NSView
- (void)setRows:(NSArray<NSView*>*)rows;
@end

@implementation SSGroup {
  NSStackView* _stack;
}
- (instancetype)init {
  if ((self = [super initWithFrame:NSZeroRect])) {
    self.translatesAutoresizingMaskIntoConstraints = NO;
    self.wantsLayer = YES;
    self.layer.cornerRadius = 11;
    self.layer.borderWidth = 1;
    _stack = vstack(@[], 0);
    [self addSubview:_stack];
    pin(_stack, self);
  }
  return self;
}
- (BOOL)wantsUpdateLayer {
  return YES;
}
- (void)updateLayer {
  // the colours follow light and dark mode (updateLayer runs again when it changes)
  BOOL dark = [[self.effectiveAppearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]] isEqualToString:NSAppearanceNameDarkAqua];
  self.layer.backgroundColor = [NSColor colorWithWhite:dark ? 1 : 0 alpha:dark ? 0.055 : 0.035].CGColor;
  self.layer.borderColor = [NSColor colorWithWhite:dark ? 1 : 0 alpha:dark ? 0.07 : 0.06].CGColor;
}
- (void)setRows:(NSArray<NSView*>*)rows {
  for (NSView* v in [_stack.arrangedSubviews copy]) [v removeFromSuperview];
  for (NSUInteger i = 0; i < rows.count; i++) {
    if (i) {
      NSBox* line = [NSBox new];
      line.boxType = NSBoxSeparator;
      sizeTo(line, 0, 1);
      [_stack addArrangedSubview:line];
      stretch(line, _stack, 12);
    }
    [_stack addArrangedSubview:rows[i]];
    stretch(rows[i], _stack);
  }
}
@end

/* ------------------------------------------------ the video view (double click: full screen) */

@interface SSVideoView : NSView
@property(copy) void (^onDoubleClick)(void);
@property(copy) void (^onMouseMove)(void);
@end
@implementation SSVideoView {
  NSTrackingArea* _tracking;
}
- (instancetype)initWithFrame:(NSRect)r {
  if ((self = [super initWithFrame:r])) {
    self.layer = [CALayer layer]; // layer-hosting: the layer first, then wantsLayer
    self.wantsLayer = YES;
    self.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
  }
  return self;
}
- (BOOL)mouseDownCanMoveWindow {
  return NO;
}
- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:self userInfo:nil];
  [self addTrackingArea:_tracking];
}
- (void)mouseMoved:(NSEvent*)e {
  if (self.onMouseMove) self.onMouseMove();
}
- (void)mouseDown:(NSEvent*)e {
  if (e.clickCount == 2 && self.onDoubleClick) self.onDoubleClick();
  if (self.onMouseMove) self.onMouseMove();
}
@end

/** Screens fill their stage, flipped so that content runs top-down. */
@interface SSFlipped : NSView
@end
@implementation SSFlipped
- (BOOL)isFlipped {
  return YES;
}
@end

/* ------------------------------------------------ one viewer in the sharer's list */

static NSString* pathName(const std::string& p) {
  static const std::map<std::string, std::string> names = {
      {"lan", "helyi hálózat"}, {"internet", "interneten át"}, {"relay", "a szerveren át"}, {"turn", "TURN relén át"}, {"tcp", "HTTPS-en át"}};
  auto it = names.find(p);
  return S(it == names.end() ? p : it->second);
}

static NSColor* latencyColor(double ms) {
  return ms < 40 ? NSColor.systemGreenColor : ms < 90 ? NSColor.systemOrangeColor : NSColor.systemRedColor;
}

@interface SSViewerRow : NSView
- (void)update:(const Json&)v;
@end

@implementation SSViewerRow {
  NSView* _dot;
  NSTextField *_name, *_sub, *_stats;
}
- (instancetype)init {
  if ((self = [super initWithFrame:NSZeroRect])) {
    self.translatesAutoresizingMaskIntoConstraints = NO;
    _dot = makeDot(NSColor.systemGreenColor);
    _name = label(@"", 13, NSFontWeightMedium, NSColor.labelColor);
    _sub = label(@"", 11, NSFontWeightRegular, NSColor.secondaryLabelColor);
    [_sub setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _stats = label(@"", 11.5, NSFontWeightRegular, NSColor.secondaryLabelColor);
    _stats.font = [NSFont monospacedDigitSystemFontOfSize:11.5 weight:NSFontWeightRegular];
    NSStackView* text = vstack(@[ _name, _sub ], 1);
    text.alignment = NSLayoutAttributeLeading;
    NSStackView* row = hstack(@[ _dot, text, spacer(), _stats ], 10);
    row.edgeInsets = NSEdgeInsetsMake(8, 12, 8, 12);
    [self addSubview:row];
    pin(row, self);
  }
  return self;
}

- (void)update:(const Json&)v {
  std::string state = v["state"].str("");
  bool connected = state == "connected";
  setDotColor(_dot, connected ? NSColor.systemGreenColor : state == "unsupported" ? NSColor.systemRedColor : state == "retrying" ? NSColor.systemOrangeColor : NSColor.tertiaryLabelColor);
  _name.stringValue = [NSString stringWithFormat:@"Néző #%d", int(v["n"].num())];
  _sub.stringValue = connected                 ? [@"közvetlen mód · " stringByAppendingString:pathName(v["path"].str(""))]
                     : state == "unsupported" ? [@"nem tud nézni: " stringByAppendingString:S(v["whyText"].str(""))]
                     : state == "retrying"    ? [@"kapcsolat újra… " stringByAppendingString:S(v["whyText"].str(""))]
                                              : @"kapcsolódik…";
  NSMutableAttributedString* s = [NSMutableAttributedString new];
  NSDictionary* plain = @{NSFontAttributeName : _stats.font, NSForegroundColorAttributeName : NSColor.secondaryLabelColor};
  auto add = [&](NSString* text, NSDictionary* attrs) {
    if (s.length) [s appendAttributedString:[[NSAttributedString alloc] initWithString:@" · " attributes:plain]];
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:attrs]];
  };
  if (connected) {
    if (v["fps"].isNumber()) add([NSString stringWithFormat:@"%d FPS", int(v["fps"].num())], plain);
    if (v["kbps"].isNumber()) add([NSString stringWithFormat:@"%.1f Mbit/s", v["kbps"].num() / 1000], plain);
    if (v["rtt"].isNumber()) add([NSString stringWithFormat:@"RTT %d ms", int(std::lround(v["rtt"].num()))], plain);
    if (v["lat"].isNumber()) {
      double lat = v["lat"].num();
      add([NSString stringWithFormat:@"~%d ms", int(std::lround(lat))],
          @{NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:11.5 weight:NSFontWeightSemibold], NSForegroundColorAttributeName : latencyColor(lat)});
    }
  }
  _stats.attributedStringValue = s;
}
@end

/* ------------------------------------------------ the app */

@interface SSApp : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate>
@end

@implementation SSApp {
  NSWindow* _window;
  NSView *_stage, *_current;
  NSView *_home, *_host, *_viewer;
  NSStackView *_homeContent, *_hostContent;
  NSPoint _anchor; // where the compact window's top centre goes back to after watching
  bool _anchored, _homeAfterFullScreen;
  // home
  NSSegmentedControl* _tabs;
  NSStackView *_sharePane, *_watchPane;
  NSSecureTextField* _password;
  NSTextField* _code;
  NSSwitch *_remember, *_audio;
  NSPopUpButton *_displayPopup, *_qualityPopup;
  NSTextField *_qualityHint, *_shareError, *_joinError;
  NSButton *_shareButton, *_joinButton, *_updateButton;
  std::vector<MacDisplay> _displays;
  // sharing
  NSView *_livePill, *_liveDot, *_statusDot;
  NSTextField *_liveText, *_roomCode, *_link, *_status, *_info, *_viewerCount;
  NSProgressIndicator* _statusSpinner;
  NSButton *_pauseButton, *_copyButton;
  SSGroup* _viewersGroup;
  NSView* _noViewers;
  std::map<std::string, SSViewerRow*> _rows;
  std::vector<std::string> _order;
  // watching
  SSVideoView* _video;
  NSView *_bar, *_overlay, *_viewDot;
  NSTextField *_viewTitle, *_viewInfo, *_overlayText;
  NSProgressIndicator* _overlaySpinner;
  NSSlider* _volume;
  NSButton* _muteButton;
  double _lastMove;

  std::unique_ptr<Engine> _engine;
  std::unique_ptr<MacHost> _hostPipe;
  std::unique_ptr<MacPlayer> _player;
  std::unique_ptr<MacAudioOut> _audioOut;
  int _session;
  bool _live, _picture, _stats, _muted, _checking, _barHidden;
  std::string _roomLink, _statusKind;
  Json _viewStats;
  NSString *_updateFile, *_updateVersion, *_savedPassword;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
  if ([self signLocallyFirst]) return;
  [self makeMenu];
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kHomeWidth + 2 * kSide, 560)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskFullSizeContentView
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"Screen Share";
  _window.titlebarAppearsTransparent = YES;
  _window.titleVisibility = NSWindowTitleHidden;
  _window.movableByWindowBackground = YES;
  _window.delegate = self;
  _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
  // the window's own background: what is behind it, blurred – as the system's panels
  NSVisualEffectView* root = [NSVisualEffectView new];
  root.material = NSVisualEffectMaterialUnderWindowBackground;
  root.blendingMode = NSVisualEffectBlendingModeBehindWindow;
  root.state = NSVisualEffectStateFollowsWindowActiveState;
  _window.contentView = root;
  _stage = [SSFlipped new];
  _stage.wantsLayer = YES;
  [root addSubview:_stage];
  pin(_stage, root);
  _live = true;
  [self makeHome];
  [self makeHost];
  [self makeViewer];
  [self show:_home];
  [self fitWindow:NO];
  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(barTick) userInfo:nil repeats:YES];
  [self checkUpdatesLater];
  __weak SSApp* weak = self;
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                        handler:^NSEvent*(NSEvent* e) {
                                          SSApp* s = weak;
                                          return s ? [s key:e] : e;
                                        }];
}

/** Not yet signed with this Mac's own identity (a new install, or an update the old way): before
 *  anything is asked, the app signs itself and starts again – a second. Tried once per build. */
- (BOOL)signLocallyFirst {
  NSString* app = NSBundle.mainBundle.bundlePath;
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  NSString* build = [NSString stringWithFormat:@"%s-%s", SS_VERSION, SS_BUILD];
  if ([app hasPrefix:@"/Volumes/"] || [app containsString:@"/AppTranslocation/"] || [[ud stringForKey:@"signTried"] isEqualToString:build]) return NO;
  if (![NSFileManager.defaultManager isWritableFileAtPath:app.stringByDeletingLastPathComponent] || signedLocally()) return NO;
  [ud setObject:build forKey:@"signTried"];
  [ud synchronize];
  if (!swapAfterQuit(nil)) return NO;
  [NSApp terminate:nil];
  return YES;
}

- (NSEvent*)key:(NSEvent*)e {
  if (e.modifierFlags & NSEventModifierFlagCommand) return e;
  if (e.window != _window || _window.attachedSheet || _current != _viewer) return e;
  NSString* k = e.charactersIgnoringModifiers.lowercaseString;
  if ([k isEqualToString:@"f"]) [_window toggleFullScreen:nil];
  else if ([k isEqualToString:@"s"]) {
    _stats = !_stats;
    [self tick];
  } else if ([k isEqualToString:@"m"]) [self setMuted:!_muted];
  else return e;
  [self moved];
  return nil;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a {
  return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)a {
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)n {
  [self endSession];
}

- (void)makeMenu {
  NSMenu* bar = [NSMenu new];
  NSMenuItem* appItem = [NSMenuItem new];
  [bar addItem:appItem];
  NSMenu* app = [NSMenu new];
  [app addItemWithTitle:@"A Screen Share névjegye" action:@selector(showAbout) keyEquivalent:@""];
  [app addItemWithTitle:@"Frissítések keresése…" action:@selector(checkUpdatesNow) keyEquivalent:@""];
  [app addItem:[NSMenuItem separatorItem]];
  [app addItemWithTitle:@"A Screen Share elrejtése" action:@selector(hide:) keyEquivalent:@"h"];
  [app addItemWithTitle:@"A többi elrejtése" action:@selector(hideOtherApplications:) keyEquivalent:@"h"].keyEquivalentModifierMask =
      NSEventModifierFlagCommand | NSEventModifierFlagOption;
  [app addItemWithTitle:@"Az összes megjelenítése" action:@selector(unhideAllApplications:) keyEquivalent:@""];
  [app addItem:[NSMenuItem separatorItem]];
  [app addItemWithTitle:@"Kilépés a Screen Share-ből" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = app;
  NSMenuItem* editItem = [NSMenuItem new];
  [bar addItem:editItem];
  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Szerkesztés"];
  [edit addItemWithTitle:@"Visszavonás" action:@selector(undo:) keyEquivalent:@"z"];
  [edit addItemWithTitle:@"Ismét" action:@selector(redo:) keyEquivalent:@"Z"];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItemWithTitle:@"Kivágás" action:@selector(cut:) keyEquivalent:@"x"];
  [edit addItemWithTitle:@"Másolás" action:@selector(copy:) keyEquivalent:@"c"];
  [edit addItemWithTitle:@"Beillesztés" action:@selector(paste:) keyEquivalent:@"v"];
  [edit addItemWithTitle:@"Az összes kijelölése" action:@selector(selectAll:) keyEquivalent:@"a"];
  editItem.submenu = edit;
  NSMenuItem* viewItem = [NSMenuItem new];
  [bar addItem:viewItem];
  NSMenu* view = [[NSMenu alloc] initWithTitle:@"Nézet"];
  [view addItemWithTitle:@"Teljes képernyő" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask =
      NSEventModifierFlagCommand | NSEventModifierFlagControl;
  viewItem.submenu = view;
  NSMenuItem* windowItem = [NSMenuItem new];
  [bar addItem:windowItem];
  NSMenu* window = [[NSMenu alloc] initWithTitle:@"Ablak"];
  [window addItemWithTitle:@"Kis méret" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
  [window addItemWithTitle:@"Nagyítás" action:@selector(performZoom:) keyEquivalent:@""];
  windowItem.submenu = window;
  NSApp.mainMenu = bar;
  NSApp.windowsMenu = window;
}

/* ---- the standard About panel: icon, name, version (build), copyright – as every Mac app's */

- (void)showAbout {
  NSMutableParagraphStyle* centred = [NSMutableParagraphStyle new];
  centred.alignment = NSTextAlignmentCenter;
  NSDictionary* small = @{
    NSFontAttributeName : [NSFont systemFontOfSize:11],
    NSForegroundColorAttributeName : NSColor.secondaryLabelColor,
    NSParagraphStyleAttributeName : centred
  };
  NSMutableAttributedString* credits = [NSMutableAttributedString new];
  NSMutableDictionary* link = [small mutableCopy];
  link[NSLinkAttributeName] = [NSURL URLWithString:kSite];
  [credits appendAttributedString:[[NSAttributedString alloc] initWithString:@"mozizas.hu" attributes:link]];
  [credits appendAttributedString:[[NSAttributedString alloc] initWithString:@"\nBearSSL (MIT) · Opus (BSD)" attributes:small]];
  [NSApp orderFrontStandardAboutPanelWithOptions:@{
    NSAboutPanelOptionApplicationName : @"Screen Share",
    NSAboutPanelOptionApplicationVersion : @SS_VERSION,
    NSAboutPanelOptionVersion : @SS_BUILD,
    NSAboutPanelOptionCredits : credits
  }];
  [NSApp activateIgnoringOtherApps:YES];
}

/* ---- the window: compact for home and sharing (as tall as its content), large for watching */

- (NSStackView*)compactContent {
  return _current == _home ? _homeContent : _current == _host ? _hostContent : nil;
}

- (NSRect)clamp:(NSRect)f {
  NSRect vis = (_window.screen ?: NSScreen.mainScreen).visibleFrame;
  f.size.width = std::min(f.size.width, vis.size.width);
  f.size.height = std::min(f.size.height, vis.size.height);
  f.origin.x = std::max(NSMinX(vis), std::min(f.origin.x, NSMaxX(vis) - f.size.width));
  f.origin.y = std::max(NSMinY(vis), std::min(f.origin.y, NSMaxY(vis) - f.size.height));
  return f;
}

- (void)setWindowFrame:(NSRect)f animate:(BOOL)animate {
  f = NSIntegralRect([self clamp:f]);
  if (NSEqualRects(f, _window.frame)) return;
  if (!animate || !_window.visible) {
    [_window setFrame:f display:YES];
    return;
  }
  NSWindow* w = _window;
  [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
    ctx.duration = 0.32;
    ctx.timingFunction = [CAMediaTimingFunction functionWithControlPoints:0.2:0.9:0.25:1];
    [w.animator setFrame:f display:YES];
  }];
}

/** The compact window as tall as what it shows, its top edge (and centre) where it was. */
- (void)fitWindow:(BOOL)animate {
  NSStackView* content = [self compactContent];
  if (!content || (_window.styleMask & NSWindowStyleMaskFullScreen)) return;
  NSSize fit = content.fittingSize;
  NSSize sz = NSMakeSize(fit.width + 2 * kSide, fit.height + kTop + kBottom);
  NSRect old = _window.frame;
  NSPoint top = _anchored ? _anchor : NSMakePoint(NSMidX(old), NSMaxY(old));
  _anchored = false;
  [self setWindowFrame:NSMakeRect(top.x - sz.width / 2, top.y - sz.height, sz.width, sz.height) animate:animate];
}

- (NSRect)viewerFrame {
  NSRect vis = (_window.screen ?: NSScreen.mainScreen).visibleFrame;
  NSString* saved = [NSUserDefaults.standardUserDefaults stringForKey:@"viewerFrame"];
  if (saved) {
    NSRect r = NSRectFromString(saved);
    if (r.size.width >= 640 && r.size.height >= 400 && NSIntersectsRect(r, vis)) return r;
  }
  CGFloat w = std::min<CGFloat>(1280, vis.size.width * 0.86), h = w * 9 / 16;
  if (h > vis.size.height * 0.86) {
    h = vis.size.height * 0.86;
    w = h * 16 / 9;
  }
  NSRect old = _window.frame;
  return NSMakeRect(NSMidX(old) - w / 2, NSMidY(old) - h / 2, w, h);
}

- (NSView*)titlebar {
  return [_window standardWindowButton:NSWindowCloseButton].superview;
}

/** Another screen: a cross-fade, and the window changes size with it. */
- (void)show:(NSView*)v {
  if (!v || v == _current) return;
  bool fromViewer = _current == _viewer, toViewer = v == _viewer;
  fade(_stage, 0.24);
  [_current removeFromSuperview];
  [_stage addSubview:v];
  pin(v, _stage);
  _current = v;
  if (toViewer) {
    _anchor = NSMakePoint(NSMidX(_window.frame), NSMaxY(_window.frame));
    _anchored = true;
    _window.styleMask = _window.styleMask | NSWindowStyleMaskResizable;
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.minSize = NSMakeSize(640, 400);
    [self setWindowFrame:[self viewerFrame] animate:YES];
    [_window makeFirstResponder:_video];
    _bar.alphaValue = 1;
    _barHidden = false;
    _lastMove = nowMs();
  } else {
    if (fromViewer) {
      [NSUserDefaults.standardUserDefaults setObject:NSStringFromRect(_window.frame) forKey:@"viewerFrame"];
      _window.styleMask = _window.styleMask & ~NSWindowStyleMaskResizable;
      _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
      _window.minSize = NSMakeSize(200, 200);
      [self titlebar].alphaValue = 1;
    }
    [self fitWindow:YES];
    [_window makeFirstResponder:nil];
  }
}

/* ---- questions: a sheet on the window */

- (void)ask:(NSString*)title text:(NSString*)text ok:(NSString*)ok cancel:(NSString*)cancel danger:(BOOL)danger action:(void (^)(void))action {
  NSAlert* a = [NSAlert new];
  a.messageText = title;
  a.informativeText = text ?: @"";
  a.alertStyle = danger ? NSAlertStyleWarning : NSAlertStyleInformational;
  NSButton* yes = [a addButtonWithTitle:ok];
  if (danger) yes.hasDestructiveAction = YES;
  if (cancel) [a addButtonWithTitle:cancel].keyEquivalent = @"\033";
  [self moved];
  [a beginSheetModalForWindow:_window
            completionHandler:^(NSModalResponse r) {
              if (r == NSAlertFirstButtonReturn && action) action();
            }];
}

/* ---- home */

- (void)makeHome {
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  NSImageView* icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
  icon.imageScaling = NSImageScaleProportionallyUpOrDown;
  sizeTo(icon, 84, 84);
  NSTextField* title = label(@"Screen Share", 22, NSFontWeightBold, NSColor.labelColor);
  NSTextField* sub = label(@"Képernyőmegosztás hanggal, a lehető legkisebb késéssel.", 13, NSFontWeightRegular, NSColor.secondaryLabelColor);
  _tabs = [NSSegmentedControl segmentedControlWithLabels:@[ @"Megosztás", @"Nézés" ] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(tabChanged)];
  _tabs.translatesAutoresizingMaskIntoConstraints = NO;
  _tabs.controlSize = NSControlSizeLarge;
  for (NSInteger i = 0; i < 2; i++) [_tabs setWidth:132 forSegment:i];

  // sharing
  _password = [NSSecureTextField new];
  _password.placeholderString = @"megosztási jelszó";
  _password.bezelStyle = NSTextFieldRoundedBezel;
  _savedPassword = loadPassword();
  _password.stringValue = _savedPassword;
  sizeTo(_password, 210, 0);
  _remember = makeSwitch(_password.stringValue.length > 0);
  SSGroup* access = [SSGroup new];
  [access setRows:@[ formRow(@"Jelszó", nil, _password), formRow(@"Jelszó megjegyzése", nil, _remember) ]];
  _displays = listDisplays();
  _displayPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  _displayPopup.autoenablesItems = NO;
  for (const MacDisplay& d : _displays) [_displayPopup.menu addItemWithTitle:S(d.name) action:nil keyEquivalent:@""]; // same names allowed
  if (_displays.empty()) {
    [_displayPopup addItemWithTitle:@"Nem találok képernyőt"];
    _displayPopup.enabled = NO;
  }
  [_displayPopup selectItemAtIndex:0];
  _qualityPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  for (const Quality& q : kQualities) [_qualityPopup addItemWithTitle:@(q.name)];
  [_qualityPopup selectItemAtIndex:std::min<NSInteger>(std::max<NSInteger>([ud integerForKey:@"quality"], 0), NSInteger(std::size(kQualities)) - 1)];
  _qualityPopup.target = self;
  _qualityPopup.action = @selector(qualityChanged);
  _audio = makeSwitch(!([ud objectForKey:@"audio"] && ![ud boolForKey:@"audio"]));
  SSGroup* picture = [SSGroup new];
  [picture setRows:@[ formRow(@"Képernyő", nil, _displayPopup), formRow(@"Minőség", nil, _qualityPopup), formRow(@"A gép hangja", @"macOS 13-tól", _audio) ]];
  _qualityHint = label(@"", 11, NSFontWeightRegular, NSColor.secondaryLabelColor, kHomeWidth - 24);
  _shareButton = button(@"Megosztás indítása", nil, self, @selector(share));
  _shareButton.controlSize = NSControlSizeLarge;
  [_shareButton.widthAnchor constraintGreaterThanOrEqualToConstant:230].active = YES;
  _shareError = label(@"", 12, NSFontWeightRegular, NSColor.systemRedColor, kHomeWidth);
  _shareError.alignment = NSTextAlignmentCenter;
  _shareError.hidden = YES;
  _sharePane = vstack(@[ access, picture, _qualityHint, _shareButton, _shareError ], 14);
  [_sharePane setCustomSpacing:6 afterView:picture];
  [_sharePane setCustomSpacing:22 afterView:_qualityHint];
  stretch(access, _sharePane);
  stretch(picture, _sharePane);
  stretch(_qualityHint, _sharePane, 12);

  // watching
  _code = [NSTextField new];
  _code.placeholderString = @"abc123 vagy link";
  _code.bezelStyle = NSTextFieldRoundedBezel;
  _code.delegate = self;
  sizeTo(_code, 220, 0);
  SSGroup* room = [SSGroup new];
  [room setRows:@[ formRow(@"Szobakód vagy link", nil, _code) ]];
  NSTextField* hint = label(@"A megosztótól kapott 6 jelű kód, vagy a teljes link (pl. mozizas.hu/#abc123).", 11, NSFontWeightRegular,
                            NSColor.secondaryLabelColor, kHomeWidth - 24);
  _joinButton = button(@"Csatlakozás", nil, self, @selector(join));
  _joinButton.controlSize = NSControlSizeLarge;
  [_joinButton.widthAnchor constraintGreaterThanOrEqualToConstant:230].active = YES;
  _joinError = label(@"", 12, NSFontWeightRegular, NSColor.systemRedColor, kHomeWidth);
  _joinError.alignment = NSTextAlignmentCenter;
  _joinError.hidden = YES;
  _watchPane = vstack(@[ room, hint, _joinButton, _joinError ], 14);
  [_watchPane setCustomSpacing:6 afterView:room];
  [_watchPane setCustomSpacing:22 afterView:hint];
  stretch(room, _watchPane);
  stretch(hint, _watchPane, 12);

  _updateButton = button(@"", @"arrow.down.circle", self, @selector(installUpdate));
  _updateButton.controlSize = NSControlSizeSmall;
  _updateButton.hidden = YES;
  _homeContent = vstack(@[ icon, title, sub, _tabs, _sharePane, _watchPane, _updateButton ], 4);
  [_homeContent setCustomSpacing:12 afterView:icon];
  [_homeContent setCustomSpacing:24 afterView:sub];
  [_homeContent setCustomSpacing:22 afterView:_tabs];
  [_homeContent setCustomSpacing:20 afterView:_sharePane];
  [_homeContent setCustomSpacing:20 afterView:_watchPane];
  stretch(_sharePane, _homeContent);
  stretch(_watchPane, _homeContent);
  _home = [self screenWith:_homeContent width:kHomeWidth];
  _tabs.selectedSegment = [ud integerForKey:@"tab"] == 1 ? 1 : 0;
  [self qualityChanged];
  [self applyTab];
}

/** A compact screen: the content at a fixed width, centred, below the title bar. */
- (NSView*)screenWith:(NSStackView*)content width:(CGFloat)width {
  NSView* screen = [SSFlipped new];
  [screen addSubview:content];
  [NSLayoutConstraint activateConstraints:@[
    [content.widthAnchor constraintEqualToConstant:width], [content.centerXAnchor constraintEqualToAnchor:screen.centerXAnchor],
    [content.topAnchor constraintEqualToAnchor:screen.topAnchor constant:kTop]
  ]];
  return screen;
}

- (void)applyTab {
  bool watch = _tabs.selectedSegment == 1;
  _sharePane.hidden = watch;
  _watchPane.hidden = !watch;
  _shareButton.keyEquivalent = watch ? @"" : @"\r"; // Return: the button of the pane shown
  _joinButton.keyEquivalent = watch ? @"\r" : @"";
}

- (void)tabChanged {
  fade(_home, 0.2);
  [self applyTab];
  [NSUserDefaults.standardUserDefaults setInteger:_tabs.selectedSegment forKey:@"tab"];
  [self fitWindow:YES];
  if (_tabs.selectedSegment == 1) [_window makeFirstResponder:_code];
  else if (!_password.stringValue.length) [_window makeFirstResponder:_password];
}

- (void)qualityChanged {
  NSInteger i = std::max<NSInteger>(_qualityPopup.indexOfSelectedItem, 0);
  _qualityHint.stringValue = @(kQualities[i].hint);
}

- (void)setError:(NSTextField*)l text:(NSString*)text {
  l.stringValue = text ?: @"";
  bool show = text.length > 0;
  l.hidden = !show;
  if (show) {
    l.alphaValue = 0;
    animateAlpha(l, 1);
  }
  [self fitWindow:YES];
}

- (void)controlTextDidChange:(NSNotification*)n {
  if (n.object == _code && !_joinError.hidden) [self setError:_joinError text:@""];
}

/* ---- sharing */

- (void)makeHost {
  _liveDot = makeDot(NSColor.systemRedColor, 7);
  _liveText = label(@"ÉLŐ", 11, NSFontWeightBold, NSColor.systemRedColor);
  NSStackView* pill = hstack(@[ _liveDot, _liveText ], 6);
  pill.edgeInsets = NSEdgeInsetsMake(3, 9, 3, 10);
  pill.wantsLayer = YES;
  pill.layer.cornerRadius = 10;
  _livePill = pill;
  NSTextField* caption = label(@"SZOBAKÓD", 11, NSFontWeightSemibold, NSColor.secondaryLabelColor);
  _roomCode = label(@"······", 40, NSFontWeightBold, NSColor.labelColor);
  _roomCode.font = [NSFont monospacedSystemFontOfSize:40 weight:NSFontWeightBold];
  _roomCode.selectable = YES;
  _link = label(@"", 12, NSFontWeightRegular, NSColor.secondaryLabelColor);
  _link.lineBreakMode = NSLineBreakByTruncatingMiddle;
  _link.selectable = YES;
  [_link.widthAnchor constraintLessThanOrEqualToConstant:kHostWidth].active = YES;
  _copyButton = button(@"Link másolása", @"doc.on.doc", self, @selector(copyLink));
  _copyButton.controlSize = NSControlSizeLarge;
  [_copyButton.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
  // how it goes
  _statusSpinner = [NSProgressIndicator new];
  _statusSpinner.style = NSProgressIndicatorStyleSpinning;
  _statusSpinner.controlSize = NSControlSizeSmall;
  _statusSpinner.displayedWhenStopped = NO;
  sizeTo(_statusSpinner, 16, 16);
  _statusDot = makeDot(NSColor.systemGreenColor);
  _status = label(@"", 13, NSFontWeightRegular, NSColor.labelColor, kHostWidth - 60);
  NSStackView* statusRow = hstack(@[ _statusSpinner, _statusDot, _status, spacer() ], 9);
  statusRow.edgeInsets = NSEdgeInsetsMake(10, 12, 10, 12);
  _info = label(@"", 11.5, NSFontWeightRegular, NSColor.secondaryLabelColor, kHostWidth - 24);
  NSStackView* infoRow = hstack(@[ _info, spacer() ], 0);
  infoRow.edgeInsets = NSEdgeInsetsMake(9, 12, 9, 12);
  SSGroup* status = [SSGroup new];
  [status setRows:@[ statusRow, infoRow ]];
  // the viewers
  _viewerCount = label(@"", 12, NSFontWeightRegular, NSColor.secondaryLabelColor);
  NSStackView* viewersHead = hstack(@[ label(@"Nézők", 13, NSFontWeightSemibold, NSColor.labelColor), spacer(), _viewerCount ], 8);
  viewersHead.edgeInsets = NSEdgeInsetsMake(0, 4, 0, 4);
  NSView* none = [NSView new];
  none.translatesAutoresizingMaskIntoConstraints = NO;
  NSTextField* noneText = label(@"Még nincs néző – küldd el a linket vagy a kódot.", 12.5, NSFontWeightRegular, NSColor.secondaryLabelColor);
  [none addSubview:noneText];
  [NSLayoutConstraint activateConstraints:@[
    [none.heightAnchor constraintEqualToConstant:56], [noneText.centerXAnchor constraintEqualToAnchor:none.centerXAnchor],
    [noneText.centerYAnchor constraintEqualToAnchor:none.centerYAnchor]
  ]];
  _noViewers = none;
  _viewersGroup = [SSGroup new];
  [_viewersGroup setRows:@[ _noViewers ]];
  _pauseButton = button(@"Szünet", @"pause.fill", self, @selector(togglePause));
  NSButton* stop = button(@"Megosztás vége", nil, self, @selector(askStop));
  stop.hasDestructiveAction = YES;
  NSStackView* buttons = hstack(@[ _pauseButton, stop ], 10);
  _hostContent = vstack(@[ _livePill, caption, _roomCode, _link, _copyButton, status, viewersHead, _viewersGroup, buttons ], 4);
  [_hostContent setCustomSpacing:14 afterView:_livePill];
  [_hostContent setCustomSpacing:0 afterView:caption];
  [_hostContent setCustomSpacing:14 afterView:_link];
  [_hostContent setCustomSpacing:24 afterView:_copyButton];
  [_hostContent setCustomSpacing:22 afterView:status];
  [_hostContent setCustomSpacing:7 afterView:viewersHead];
  [_hostContent setCustomSpacing:22 afterView:_viewersGroup];
  stretch(status, _hostContent);
  stretch(viewersHead, _hostContent);
  stretch(_viewersGroup, _hostContent);
  _host = [self screenWith:_hostContent width:kHostWidth];
}

/* ---- watching */

- (void)makeViewer {
  __weak SSApp* weak = self;
  _viewer = [SSFlipped new];
  _viewer.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]; // over the picture: always dark
  _video = [[SSVideoView alloc] initWithFrame:NSZeroRect];
  _video.onDoubleClick = ^{ [weak fullScreen]; };
  _video.onMouseMove = ^{ [weak moved]; };
  [_viewer addSubview:_video];
  pin(_video, _viewer);
  // while there is no picture: a spinner and what is happening
  _overlaySpinner = [NSProgressIndicator new];
  _overlaySpinner.style = NSProgressIndicatorStyleSpinning;
  _overlaySpinner.displayedWhenStopped = NO;
  sizeTo(_overlaySpinner, 32, 32);
  _overlayText = label(@"", 14, NSFontWeightMedium, [NSColor colorWithWhite:1 alpha:0.85], 480);
  _overlayText.alignment = NSTextAlignmentCenter;
  _overlay = vstack(@[ _overlaySpinner, _overlayText ], 14);
  [_viewer addSubview:_overlay];
  [NSLayoutConstraint activateConstraints:@[
    [_overlay.centerXAnchor constraintEqualToAnchor:_viewer.centerXAnchor], [_overlay.centerYAnchor constraintEqualToAnchor:_viewer.centerYAnchor]
  ]];
  // the controls: a floating glass capsule at the bottom, it fades away while the mouse rests
  _viewDot = makeDot(NSColor.systemOrangeColor);
  _viewTitle = label(@"", 13, NSFontWeightSemibold, NSColor.labelColor);
  _viewInfo = label(@"", 12, NSFontWeightRegular, NSColor.secondaryLabelColor);
  _viewInfo.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
  [_viewInfo setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  _muteButton = iconButton(@"speaker.wave.2.fill", self, @selector(toggleMute));
  _volume = [NSSlider sliderWithValue:100 minValue:0 maxValue:100 target:self action:@selector(volumeChanged)];
  _volume.controlSize = NSControlSizeSmall;
  sizeTo(_volume, 96, 0);
  NSButton* full = iconButton(@"arrow.up.left.and.arrow.down.right", self, @selector(fullScreen));
  NSButton* leave = button(@"Kilépés", nil, self, @selector(stopToHome));
  leave.hasDestructiveAction = YES;
  NSStackView* row = hstack(@[ _viewDot, _viewTitle, _viewInfo, _muteButton, _volume, full, leave ], 10);
  [row setCustomSpacing:14 afterView:_viewInfo];
  [row setCustomSpacing:4 afterView:_muteButton];
  [row setCustomSpacing:14 afterView:full];
  row.edgeInsets = NSEdgeInsetsMake(0, 20, 0, 9);
  _bar = glassCapsule(row, 25);
  [_viewer addSubview:_bar];
  [NSLayoutConstraint activateConstraints:@[
    [_bar.heightAnchor constraintEqualToConstant:50], [_bar.centerXAnchor constraintEqualToAnchor:_viewer.centerXAnchor],
    [_bar.bottomAnchor constraintEqualToAnchor:_viewer.bottomAnchor constant:-22],
    [_bar.widthAnchor constraintLessThanOrEqualToAnchor:_viewer.widthAnchor constant:-40]
  ]];
}

- (void)moved {
  _lastMove = nowMs();
  if (_barHidden) {
    _barHidden = false;
    animateAlpha(_bar, 1, 0.18);
    if (!(_window.styleMask & NSWindowStyleMaskFullScreen)) animateAlpha([self titlebar], 1, 0.18);
  }
}

- (void)barTick {
  // the bar (and the window's buttons) fade away while the picture plays and the mouse rests
  if (_current != _viewer || !_picture || _barHidden || _window.attachedSheet) return;
  NSPoint m = [_bar convertPoint:_window.mouseLocationOutsideOfEventStream fromView:nil];
  if (NSPointInRect(m, _bar.bounds) || nowMs() - _lastMove < 2600) return;
  _barHidden = true;
  animateAlpha(_bar, 0, 0.5);
  if (!(_window.styleMask & NSWindowStyleMaskFullScreen)) animateAlpha([self titlebar], 0, 0.5);
  if (_window.styleMask & NSWindowStyleMaskFullScreen) [NSCursor setHiddenUntilMouseMoves:YES];
}

/* ---- sessions */

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
    [self setError:_shareError text:@"Add meg a megosztási jelszót."];
    [_window makeFirstResponder:_password];
    return;
  }
  if (_displays.empty()) {
    [self setError:_shareError text:@"Nem találok képernyőt."];
    return;
  }
  NSInteger quality = std::max<NSInteger>(_qualityPopup.indexOfSelectedItem, 0);
  NSInteger monitor = std::max<NSInteger>(_displayPopup.indexOfSelectedItem, 0);
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  [ud setInteger:quality forKey:@"quality"];
  [ud setBool:_audio.state == NSControlStateValueOn forKey:@"audio"];
  NSString* keep = _remember.state == NSControlStateValueOn ? pw : @"";
  if (![keep isEqualToString:_savedPassword]) { // the keychain only when it changes
    savePassword(keep);
    _savedPassword = keep;
  }
  _shareError.hidden = YES;
  [self endSession];
  int session = ++_session;
  EngineConfig cfg;
  cfg.server = kServer;
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
  const Quality& q = kQualities[quality];
  MacHost::Options o;
  if (monitor < NSInteger(_displays.size())) o.display = _displays[size_t(monitor)].id;
  o.maxWidth = q.w;
  o.maxHeight = q.h;
  o.fps = q.fps;
  o.audio = _audio.state == NSControlStateValueOn;
  _hostPipe->start(o);
  _live = true;
  _roomLink.clear();
  _statusKind.clear();
  _roomCode.stringValue = @"······";
  _link.stringValue = @"A link a kapcsolódás után jelenik meg…";
  _copyButton.enabled = NO;
  _info.stringValue = @"A kép indul…";
  [self setStatus:@"Kapcsolódás a szerverhez…" kind:""];
  [self updateBadge];
  [self updateViewers:Json::array()];
  _pauseButton.title = @"Szünet";
  _pauseButton.image = symbol(@"pause.fill", 13);
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
    [self setError:_joinError text:@"Érvénytelen szobakód – írd be a 6 jelű kódot, vagy illeszd be a teljes linket."];
    [_window makeFirstResponder:_code];
    return;
  }
  _joinError.hidden = YES;
  [self endSession];
  int session = ++_session;
  EngineConfig cfg;
  cfg.server = kServer;
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
  _overlayText.stringValue = @"Csatlakozás a szobához…";
  _overlay.alphaValue = 1;
  [_overlaySpinner startAnimation:nil];
  _viewTitle.stringValue = S("Szoba: " + code);
  _viewInfo.stringValue = @"";
  setDotColor(_viewDot, NSColor.systemOrangeColor);
  [self show:_viewer];
  engine->join(code);
}

- (void)stopToHome {
  [self endSession];
  [_overlaySpinner stopAnimation:nil];
  if (_window.styleMask & NSWindowStyleMaskFullScreen) {
    _homeAfterFullScreen = true; // the window first leaves full screen, then shrinks
    [_window toggleFullScreen:nil];
    return;
  }
  [self show:_home];
}

- (void)askStop {
  __weak SSApp* weak = self;
  [self ask:@"Befejezed a megosztást?" text:@"A nézőknél megáll a kép, és a szoba bezárul." ok:@"Befejezés" cancel:@"Mégse" danger:YES action:^{
    [weak stopToHome];
  }];
}

- (void)togglePause {
  _live = !_live;
  if (_engine) _engine->setLive(_live);
  _pauseButton.title = _live ? @"Szünet" : @"Folytatás";
  _pauseButton.image = symbol(_live ? @"pause.fill" : @"play.fill", 13);
  [self updateBadge];
}

- (void)updateBadge {
  NSColor* c = _live ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
  _liveText.stringValue = _live ? @"ÉLŐ" : @"SZÜNETEL";
  _liveText.textColor = c;
  setDotColor(_liveDot, c);
  _livePill.layer.backgroundColor = [c colorWithAlphaComponent:0.15].CGColor;
}

- (void)copyLink {
  if (_roomLink.empty()) return;
  [NSPasteboard.generalPasteboard clearContents];
  [NSPasteboard.generalPasteboard setString:S(_roomLink) forType:NSPasteboardTypeString];
  _copyButton.title = @"Kimásolva";
  _copyButton.image = symbol(@"checkmark", 13, NSFontWeightSemibold);
  __weak NSButton* b = _copyButton;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    b.title = @"Link másolása";
    b.image = symbol(@"doc.on.doc", 13);
  });
}

- (void)fullScreen {
  [_window toggleFullScreen:nil];
}

- (void)volumeChanged {
  [self setMuted:false];
}

- (void)toggleMute {
  [self setMuted:!_muted];
}

- (void)setMuted:(bool)m {
  _muted = m;
  if (_audioOut) _audioOut->setVolume(m ? 0.0f : float(_volume.doubleValue / 100));
  _muteButton.image = symbol(m ? @"speaker.slash.fill" : @"speaker.wave.2.fill", 15);
}

/* ---- engine events (main thread) */

- (void)setStatus:(NSString*)text kind:(const std::string&)kind {
  _statusKind = kind;
  _status.stringValue = text ?: @"";
  bool connecting = kind.empty();
  if (connecting) [_statusSpinner startAnimation:nil];
  else [_statusSpinner stopAnimation:nil];
  _statusSpinner.hidden = !connecting;
  _statusDot.hidden = connecting;
  NSColor* c = kind == "ok" ? NSColor.systemGreenColor : kind == "warn" ? NSColor.systemOrangeColor : kind == "error" ? NSColor.systemRedColor : NSColor.labelColor;
  _status.textColor = kind == "error" ? NSColor.systemRedColor : NSColor.labelColor;
  if (!connecting) setDotColor(_statusDot, c);
  [self fitWindow:YES];
}

- (void)updateViewers:(const Json&)list {
  std::vector<Json> items = list.items();
  std::sort(items.begin(), items.end(), [](const Json& a, const Json& b) { return a["n"].num() < b["n"].num(); });
  std::vector<std::string> order;
  for (const Json& v : items) order.push_back(v["id"].str(""));
  for (auto it = _rows.begin(); it != _rows.end();)
    it = std::find(order.begin(), order.end(), it->first) == order.end() ? _rows.erase(it) : std::next(it);
  NSMutableArray<NSView*>* rows = [NSMutableArray array];
  for (const Json& v : items) {
    std::string id = v["id"].str("");
    SSViewerRow* row = _rows.count(id) ? _rows[id] : nil;
    if (!row) {
      row = [SSViewerRow new];
      row.alphaValue = 0;
      animateAlpha(row, 1, 0.3);
      _rows[id] = row;
    }
    [row update:v];
    [rows addObject:row];
  }
  if (order != _order) { // someone came or went: the list anew, the window as tall as it
    _order = order;
    [_viewersGroup setRows:rows.count ? rows : @[ _noViewers ]];
    [self fitWindow:YES];
  }
  _viewerCount.stringValue = items.empty() ? @"" : [NSString stringWithFormat:@"%d", int(items.size())];
}

- (void)onEvent:(const Json&)e {
  const std::string& type = e["type"].str();
  if (type == "status") {
    [self setStatus:S(e["text"].str()) kind:e["kind"].str()];
  } else if (type == "hosted") {
    _roomCode.attributedStringValue = [[NSAttributedString alloc] initWithString:S(e["code"].str()) attributes:@{
      NSFontAttributeName : [NSFont monospacedSystemFontOfSize:40 weight:NSFontWeightBold],
      NSForegroundColorAttributeName : NSColor.labelColor,
      NSKernAttributeName : @5
    }];
    _roomLink = e["link"].str();
    _link.stringValue = S(_roomLink);
    _copyButton.enabled = YES;
    if (e["changed"].b())
      [self ask:@"Új link készült" text:@"A korábbi szoba lejárt, ezért új kód és link készült – küldd el újra a nézőknek." ok:@"Rendben" cancel:nil danger:NO action:nil];
  } else if (type == "host-error") {
    std::string c = e["code"].str();
    NSString* text = c == "auth" ? @"Hibás megosztási jelszó." : c == "rate" ? @"Túl sok próbálkozás, várj egy kicsit." : c == "full" ? @"A szerver megtelt, próbáld később."
                                                                                                                                      : S("Nem érem el a szervert (" + c + ").");
    [self endSession];
    [self show:_home];
    [self setError:_shareError text:text];
  } else if (type == "viewers") {
    [self updateViewers:e["list"]];
  } else if (type == "closed" && _current == _host) {
    [self stopToHome];
    [self setError:_shareError text:@"A szobát egy másik ablak vette át."];
  } else if (type == "view-state" && _current == _viewer) {
    bool picture = e["picture"].b();
    if (picture != _picture) {
      _picture = picture;
      setDotColor(_viewDot, picture ? NSColor.systemGreenColor : NSColor.systemOrangeColor);
      if (picture) {
        animateAlpha(_overlay, 0, 0.25);
        [_overlaySpinner stopAnimation:nil];
        _lastMove = nowMs();
      } else {
        [_overlaySpinner startAnimation:nil];
        animateAlpha(_overlay, 1, 0.25);
        [self moved];
      }
    }
    if (!picture) _overlayText.stringValue = S(e["text"].str());
  } else if (type == "view-stats") {
    _viewStats = e;
  }
}

- (void)tick {
  if (_current == _host && _hostPipe) {
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
    } else {
      s = "A kép indul…";
    }
    s += i.audioLayout.empty() ? " · hang nélkül" : " · hang: " + i.audioLayout;
    NSString* text = S(s);
    if (![text isEqualToString:_info.stringValue]) {
      _info.stringValue = text;
      _info.textColor = i.error.empty() ? NSColor.secondaryLabelColor : NSColor.systemRedColor;
      [self fitWindow:YES];
    }
  } else if (_current == _viewer && _player) {
    MacPlayer::Stats st = _player->stats();
    if (_engine && st.latencyMs >= 0) _engine->reportLatency(st.latencyMs, st.fps);
    std::string s;
    if (_picture && st.width) {
      char b[200];
      snprintf(b, sizeof b, "%d×%d · %.0f FPS", st.width, st.height, st.fps);
      s = b;
      if (st.latencyMs >= 0) s += " · késés " + std::to_string(int(std::lround(st.latencyMs))) + " ms";
      const Json& v = _viewStats;
      if (v["path"].isString()) s += " · " + U(pathName(v["path"].str()));
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

- (void)checkUpdatesLater {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    [self checkUpdates:NO];
  });
}

- (void)checkUpdatesNow {
  [self checkUpdates:YES];
}

- (void)checkUpdates:(BOOL)asked {
  if (_checking) return;
  _checking = true;
  NSURL* url = [NSURL URLWithString:[kFeed stringByAppendingString:@"/native-latest.json"]];
  NSURLRequest* req = [NSURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:20];
  [[NSURLSession.sharedSession dataTaskWithRequest:req
                                 completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                                   NSString* version = nil;
                                   NSString* file = nil;
                                   if (data && !error) {
                                     Json j = Json::parse(std::string(static_cast<const char*>(data.bytes), data.length));
                                     std::string v = j["version"].str(""), f = j["mac"].str("");
                                     if (!v.empty() && !f.empty() && [S(v) compare:@SS_VERSION options:NSNumericSearch] == NSOrderedDescending) {
                                       version = S(v);
                                       file = S(f);
                                     }
                                   }
                                   dispatch_async(dispatch_get_main_queue(), ^{
                                     [self updateChecked:version file:file failed:error != nil asked:asked];
                                   });
                                 }] resume];
}

- (void)updateChecked:(NSString*)version file:(NSString*)file failed:(BOOL)failed asked:(BOOL)asked {
  _checking = false;
  if (version) {
    _updateVersion = version;
    _updateFile = file;
    _updateButton.title = [NSString stringWithFormat:@"Frissítés: %@", version];
    if (_updateButton.hidden) {
      _updateButton.alphaValue = 0;
      _updateButton.hidden = NO;
      animateAlpha(_updateButton, 1, 0.3);
      [self fitWindow:YES];
    }
  }
  if (!asked) return;
  __weak SSApp* weak = self;
  if (version)
    [self ask:[NSString stringWithFormat:@"Elérhető a Screen Share %@", version]
         text:[NSString stringWithFormat:@"A telepített verzió: %@. A frissítés letöltődik, majd a Screen Share újraindul.", @SS_VERSION]
           ok:@"Frissítés"
       cancel:@"Később"
       danger:NO
       action:^{
         [weak installUpdate];
       }];
  else if (failed)
    [self ask:@"Nem érem el a frissítéseket" text:@"Ellenőrizd az internetkapcsolatot, és próbáld újra később." ok:@"OK" cancel:nil danger:NO action:nil];
  else
    [self ask:@"Ez a legújabb verzió" text:[NSString stringWithFormat:@"A Screen Share %@ a legfrissebb.", @SS_VERSION] ok:@"OK" cancel:nil danger:NO action:nil];
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
                                        [self ask:@"A frissítés nem sikerült" text:@"Nem tudtam letölteni az új verziót. Próbáld újra később." ok:@"OK" cancel:nil danger:NO action:nil];
                                        return;
                                      }
                                      // after we quit: the new app (signed with this Mac's identity) in place of this one, then start it
                                      if (swapAfterQuit(fresh)) {
                                        [NSApp terminate:nil];
                                        return;
                                      }
                                      NSString* target = NSBundle.mainBundle.bundlePath;
                                      NSString* script = [NSString
                                          stringWithFormat:@"while kill -0 %d 2>/dev/null; do sleep 0.2; done; rm -rf \"%@\" && mv \"%@\" \"%@\" && xattr -dr com.apple.quarantine \"%@\" 2>/dev/null; open \"%@\"",
                                                           NSProcessInfo.processInfo.processIdentifier, target, fresh, target, target, target];
                                      [NSTask launchedTaskWithLaunchPath:@"/bin/sh" arguments:@[ @"-c", script ]];
                                      [NSApp terminate:nil];
                                    });
                                  }] resume];
}

/* ---- window */

- (BOOL)windowShouldClose:(NSWindow*)w {
  if (_current != _host) return YES;
  __weak NSWindow* win = w;
  __weak SSApp* weak = self;
  [self ask:@"Kilépsz?" text:@"A megosztás befejeződik: a nézőknél megáll a kép, és a szoba bezárul." ok:@"Kilépés" cancel:@"Mégse" danger:YES action:^{
    [weak endSession];
    [win close];
  }];
  return NO;
}

- (void)windowDidExitFullScreen:(NSNotification*)n {
  if (_homeAfterFullScreen) {
    _homeAfterFullScreen = false;
    if (!_engine) [self show:_home];
    return;
  }
  [self moved];
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
    result.set("audioBuffers", i.audioBuffers).set("audioPackets", i.audioPackets).set("audioNote", i.audioNote);
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
