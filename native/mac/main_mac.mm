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
#include <climits>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "clock.h"
#include "engine.h"
#include "host_mac.h"
#include "player_mac.h"
#include "ws_bench.h"

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
// closing the sharing's window, quitting while sharing: its room waits for the app
static NSString* const kKeptText = @"A szoba 10 percig megmarad, a nézők várnak benne: ha addig újra elindítod a Screen Share-t, ugyanott folytatódik a megosztás. "
                                   @"Végleg a Megosztás vége gombbal zárhatod be.";

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

// The keychain ties an item to the build that saved it: for an app signed without an Apple team its
// "partition" is the code's hash, so after every update macOS asked for the login password. The item
// is saved and read by Apple's security tool instead – one and the same program for every version of
// this app, and so never asked about. (In it: the password's UTF-8 as hex – printable, no quoting.)
static NSString* const kKeychainService = @"hu.mozizas.screenshare";

/** /usr/bin/security with `args` (and `input` on its standard input): what it printed, its exit
 *  status in `status`, and in `errors` what it printed as errors. */
static NSString* runSecurity(NSArray<NSString*>* args, NSString* input, int* status, NSString** errors = nullptr) {
  *status = -1;
  NSTask* t = [NSTask new];
  t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/security"];
  t.arguments = args;
  NSPipe* out = [NSPipe pipe];
  NSPipe* err = [NSPipe pipe];
  NSPipe* in = input ? [NSPipe pipe] : nil;
  t.standardOutput = out;
  t.standardError = err;
  if (in) t.standardInput = in;
  else t.standardInput = NSFileHandle.fileHandleWithNullDevice;
  if (![t launchAndReturnError:nil]) return @"";
  if (in) {
    [in.fileHandleForWriting writeData:[input dataUsingEncoding:NSUTF8StringEncoding]];
    [in.fileHandleForWriting closeFile];
  }
  // (both read at once: a pipe that fills up would stop the tool)
  __block NSData* errData = nil;
  dispatch_group_t g = dispatch_group_create();
  dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    errData = [err.fileHandleForReading readDataToEndOfFile];
  });
  NSData* data = [out.fileHandleForReading readDataToEndOfFile];
  dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
  [t waitUntilExit];
  *status = t.terminationStatus;
  if (errors) *errors = [[NSString alloc] initWithData:errData ?: [NSData data] encoding:NSUTF8StringEncoding] ?: @"";
  return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
}

/** false (and why in `why`) when it could not be saved. */
static bool savePassword(NSString* pw, NSString** why = nullptr) {
  NSData* utf8 = [pw dataUsingEncoding:NSUTF8StringEncoding];
  NSMutableString* hex = [NSMutableString stringWithCapacity:utf8.length * 2];
  for (NSUInteger i = 0; i < utf8.length; i++) [hex appendFormat:@"%02x", ((const uint8_t*)utf8.bytes)[i]];
  // on its standard input (`-i`), not on the command line, where other programs could see it. (No -T:
  // the item's access stays the tool's own – setting it on an existing item is what asks.)
  NSString* cmd = pw.length ? [NSString stringWithFormat:@"add-generic-password -U -s %@ -a share -w %@\n", kKeychainService, hex]
                            : [NSString stringWithFormat:@"delete-generic-password -s %@ -a share\n", kKeychainService];
  int status = 0;
  NSString* errors = @"";
  runSecurity(@[ @"-i" ], cmd, &status, &errors);
  // (-i ends well whatever a command did; a command that failed says so: "…: returned N")
  bool ok = status == 0 && (!pw.length || ![errors containsString:@"returned"]); // (nothing to delete: fine)
  if (why) *why = errors;
  return ok;
}

static NSString* loadPassword() {
  int status = 0;
  NSString* hex = [runSecurity(@[ @"find-generic-password", @"-s", kKeychainService, @"-a", @"share", @"-w" ], nil, &status)
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (status != 0 || hex.length % 2) return @"";
  NSMutableData* utf8 = [NSMutableData dataWithLength:hex.length / 2];
  for (NSUInteger i = 0; i < utf8.length; i++) {
    unsigned byte = 0;
    if (sscanf([hex substringWithRange:NSMakeRange(i * 2, 2)].UTF8String, "%2x", &byte) != 1) return @"";
    ((uint8_t*)utf8.mutableBytes)[i] = (uint8_t)byte;
  }
  return [[NSString alloc] initWithData:utf8 encoding:NSUTF8StringEncoding] ?: @"";
}

/** The item earlier versions saved themselves (each version in a partition of its own): removed –
 *  never read, which is what asked for the login password. Without any question: where it would
 *  take one, it stays. */
static void dropOldPassword() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  Boolean ui = true;
  SecKeychainGetUserInteractionAllowed(&ui);
  SecKeychainSetUserInteractionAllowed(false);
  NSDictionary* q = @{(__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword, (__bridge id)kSecAttrService : @"ScreenShare", (__bridge id)kSecAttrAccount : @"share"};
  SecItemDelete((__bridge CFDictionaryRef)q);
  SecKeychainSetUserInteractionAllowed(ui);
#pragma clang diagnostic pop
}

/* ------------------------------------------------ the rooms this app is in: rooms = {code: {host, token, pid, at}} */

// kept in its settings while it is in them: after a crash, an update or the Mac restarting the app goes
// back into them (the server keeps a room this long for its sharer)
static const double kRoomKeepS = 600;

struct SavedRoom {
  std::string code, token;
  bool host = false;
  int pid = 0;
  double at = 0; // (seconds since 1970: when the app was last surely in it)
};

static void saveRoom(const std::string& code, bool host, const std::string& token) {
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  NSMutableDictionary* rooms = [[ud dictionaryForKey:@"rooms"] mutableCopy] ?: [NSMutableDictionary dictionary];
  rooms[S(code)] = @{@"host" : @(host), @"token" : S(token), @"pid" : @(getpid()), @"at" : @(NSDate.date.timeIntervalSince1970)};
  [ud setObject:rooms forKey:@"rooms"];
}

static void forgetRoom(const std::string& code) {
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  NSMutableDictionary* rooms = [[ud dictionaryForKey:@"rooms"] mutableCopy];
  if (code.empty() || !rooms[S(code)]) return;
  [rooms removeObjectForKey:S(code)];
  [ud setObject:rooms forKey:@"rooms"];
}

static std::vector<SavedRoom> savedRooms() {
  std::vector<SavedRoom> out;
  NSDictionary* rooms = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"rooms"];
  for (NSString* code in rooms) {
    NSDictionary* r = rooms[code];
    if (![r isKindOfClass:NSDictionary.class]) continue;
    SavedRoom s;
    s.code = U(code);
    s.host = [r[@"host"] boolValue];
    s.token = [r[@"token"] isKindOfClass:NSString.class] ? U(r[@"token"]) : "";
    s.pid = [r[@"pid"] intValue];
    s.at = [r[@"at"] doubleValue];
    out.push_back(s);
  }
  return out;
}

/** Is that process another copy of this app, still running (its rooms are its own)? */
static bool otherCopy(int pid) {
  if (pid <= 0 || pid == getpid()) return false;
  NSRunningApplication* a = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
  return a && !a.terminated && [a.bundleIdentifier isEqualToString:NSBundle.mainBundle.bundleIdentifier];
}

/** Quitting because the Mac shuts down, restarts or logs out (not ⌘Q): the quit event's reason. */
static bool systemQuit() {
  auto code = [](const char* s) { return FourCharCode(uint8_t(s[0])) << 24 | FourCharCode(uint8_t(s[1])) << 16 | FourCharCode(uint8_t(s[2])) << 8 | FourCharCode(uint8_t(s[3])); };
  NSAppleEventDescriptor* e = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
  if (!e || e.eventClass != code("aevt") || e.eventID != code("quit")) return false;
  FourCharCode why = [e attributeDescriptorForKeyword:code("why?")].typeCodeValue;
  return why == code("shut") || why == code("rest") || why == code("rlgo") || why == code("logo");
}

/* ------------------------------------------------ this Mac's own signature (sign_local.sh) */

// macOS keeps what was allowed (screen and sound recording, the local network) for the app's signing
// identity. The builds are signed ad hoc – another identity for every version – so the
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
  std::string name = v["name"].str("");
  _name.stringValue = name.empty() ? [NSString stringWithFormat:@"Néző #%d", int(v["n"].num())] : S(name);
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

/* ------------------------------------------------ the room's chat */

/** A colour of its own for each writer – the website's and the Windows app's: hsl(hash, 80%, 74%). */
static NSColor* nameColor(const std::string& from) {
  uint32_t h = 0;
  for (unsigned char ch : from) h = h * 31 + ch;
  h *= 2654435761u; // (the golden angle apart: viewers v7 and v8 far apart)
  double hue = std::floor(double(h) / 4294967296.0 * 360.0) / 60.0, l = 0.74, c = (1 - std::fabs(2 * l - 1)) * 0.8;
  double x = c * (1 - std::fabs(std::fmod(hue, 2.0) - 1)), m = l - c / 2;
  const double v[6][3] = {{c, x, 0}, {x, c, 0}, {0, c, x}, {0, x, c}, {x, 0, c}, {c, 0, x}};
  const double* p = v[std::min(5, int(hue))];
  return [NSColor colorWithSRGBRed:p[0] + m green:p[1] + m blue:p[2] + m alpha:1];
}

static NSColor* chatAccent() { return [NSColor colorWithSRGBRed:0xF7 / 255.0 green:0x4F / 255.0 blue:0x9F / 255.0 alpha:1]; }

/** The chat (sharing and watching): the messages – the newest at the bottom, selectable –, the box to
 *  write in with a send button, the name under it, a note (too fast, not sent…). */
@interface SSChatView : NSView <NSTextFieldDelegate>
@property(nonatomic, copy) void (^onSend)(NSString* text);
@property(nonatomic, copy) void (^onName)(NSString* name);
- (instancetype)initWithEmpty:(NSString*)empty;
- (void)history:(const Json&)list;
- (void)add:(const Json&)m;
- (void)note:(NSString*)text lasting:(BOOL)lasting;
- (void)serverName:(NSString*)name;
- (void)restore:(NSString*)text;
- (void)clear;
- (void)focusInput;
- (NSString*)shown;
- (void)typeAndSend:(NSString*)text;
@end

@implementation SSChatView {
  NSScrollView* _scroll;
  NSTextView* _list;
  NSTextField *_input, *_name, *_note, *_empty;
  std::deque<NSUInteger> _lengths; // each message's characters in the list (the oldest go after 200)
  NSTimer* _noteTimer;
  NSDateFormatter* _clock;
}

- (instancetype)initWithEmpty:(NSString*)empty {
  if (!(self = [super initWithFrame:NSZeroRect])) return nil;
  self.translatesAutoresizingMaskIntoConstraints = NO;
  _clock = [NSDateFormatter new];
  _clock.dateFormat = @"HH:mm";
  _scroll = [NSScrollView new];
  _scroll.translatesAutoresizingMaskIntoConstraints = NO;
  _scroll.drawsBackground = NO;
  _scroll.hasVerticalScroller = YES;
  _scroll.autohidesScrollers = YES;
  _list = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 300, 100)];
  _list.editable = NO;
  _list.selectable = YES;
  _list.drawsBackground = NO;
  _list.textContainerInset = NSMakeSize(8, 8);
  _list.verticallyResizable = YES;
  _list.horizontallyResizable = NO;
  _list.autoresizingMask = NSViewWidthSizable;
  _list.textContainer.widthTracksTextView = YES;
  _scroll.documentView = _list;
  _empty = label(empty, 12.5, NSFontWeightRegular, NSColor.secondaryLabelColor, 250);
  _empty.alignment = NSTextAlignmentCenter;
  _input = [NSTextField new];
  _input.placeholderString = @"Üzenet…";
  _input.bezelStyle = NSTextFieldRoundedBezel;
  _input.delegate = self;
  _input.target = self;
  _input.action = @selector(send);
  NSButton* send = iconButton(@"paperplane.fill", self, @selector(send));
  send.toolTip = @"Küldés (Enter)";
  NSStackView* inputRow = hstack(@[ _input, send ], 8);
  _name = [NSTextField new];
  _name.placeholderString = @"a neved a chatben";
  _name.bezelStyle = NSTextFieldRoundedBezel;
  _name.controlSize = NSControlSizeSmall;
  _name.font = [NSFont systemFontOfSize:12];
  _name.delegate = self;
  _name.stringValue = [NSUserDefaults.standardUserDefaults stringForKey:@"chatName"] ?: @"";
  NSStackView* nameRow = hstack(@[ label(@"Neved", 12, NSFontWeightRegular, NSColor.secondaryLabelColor), _name ], 8);
  _note = label(@"", 11.5, NSFontWeightRegular, NSColor.systemOrangeColor, 260);
  _note.hidden = YES;
  NSStackView* bottom = vstack(@[ inputRow, nameRow, _note ], 8);
  bottom.alignment = NSLayoutAttributeLeading;
  stretch(inputRow, bottom);
  stretch(nameRow, bottom);
  for (NSView* v in @[ _scroll, _empty, bottom ]) [self addSubview:v];
  [NSLayoutConstraint activateConstraints:@[
    [_scroll.topAnchor constraintEqualToAnchor:self.topAnchor], [_scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
    [_scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor], [_scroll.bottomAnchor constraintEqualToAnchor:bottom.topAnchor constant:-10],
    [bottom.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:12], [bottom.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-12],
    [bottom.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-12], [_empty.centerXAnchor constraintEqualToAnchor:_scroll.centerXAnchor],
    [_empty.centerYAnchor constraintEqualToAnchor:_scroll.centerYAnchor]
  ]];
  return self;
}

- (void)send {
  NSString* text = [_input.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (!text.length) return;
  _input.stringValue = @"";
  if (self.onSend) self.onSend(text);
}

- (void)typeAndSend:(NSString*)text { // (CI)
  _input.stringValue = text;
  [self send];
}

/** Esc leaves the boxes (back to the picture's keys); the name is kept when it is left. */
- (BOOL)control:(NSControl*)c textView:(NSTextView*)tv doCommandBySelector:(SEL)command {
  if (command == @selector(cancelOperation:)) {
    [self.window makeFirstResponder:self.window.contentView];
    return YES;
  }
  return NO;
}

- (void)controlTextDidEndEditing:(NSNotification*)n {
  if (n.object != _name) return;
  NSString* name = [_name.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
  if (name.length > 24) name = [name substringToIndex:24];
  _name.stringValue = name;
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  if ([name isEqualToString:[ud stringForKey:@"chatName"] ?: @""]) return;
  [ud setObject:name forKey:@"chatName"];
  if (self.onName) self.onName(name);
}

- (NSAttributedString*)line:(const Json&)m first:(BOOL)first {
  std::string from = m["from"].str();
  bool host = m["host"].b(false), mine = m["mine"].b(false);
  NSMutableParagraphStyle* para = [NSMutableParagraphStyle new];
  para.paragraphSpacingBefore = first ? 0 : 9;
  NSMutableAttributedString* s = [NSMutableAttributedString new];
  auto add = [&](NSString* text, NSDictionary* attrs) {
    NSMutableDictionary* a = [attrs mutableCopy];
    a[NSParagraphStyleAttributeName] = para;
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:a]];
  };
  if (!first) add(@"\n", @{NSFontAttributeName : [NSFont systemFontOfSize:4]});
  add(S(m["name"].str()), @{NSFontAttributeName : [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold], NSForegroundColorAttributeName : host ? chatAccent() : nameColor(from)});
  NSString* tag = host ? (mine ? @"  megosztó (te)" : @"  megosztó") : mine ? @"  (te)" : @"";
  NSDictionary* small = @{NSFontAttributeName : [NSFont systemFontOfSize:10.5], NSForegroundColorAttributeName : NSColor.tertiaryLabelColor};
  add(tag, small);
  if (m["at"].isNumber()) add([@"  " stringByAppendingString:[_clock stringFromDate:[NSDate dateWithTimeIntervalSince1970:m["at"].num() / 1000]]], small);
  // (the system's own label colour: one changed with alpha is fixed to the light look – dark on dark)
  add([@"\n" stringByAppendingString:S(m["text"].str())], @{NSFontAttributeName : [NSFont systemFontOfSize:13], NSForegroundColorAttributeName : NSColor.labelColor});
  return s;
}

- (void)history:(const Json&)list {
  [self clear];
  for (const Json& m : list.items()) [self add:m];
}

- (void)add:(const Json&)m {
  BOOL atEnd = NSMaxY(_scroll.documentVisibleRect) >= NSMaxY(_list.bounds) - 40;
  NSAttributedString* s = [self line:m first:_lengths.empty()];
  [_list.textStorage appendAttributedString:s];
  _lengths.push_back(s.length);
  if (_lengths.size() > 200) { // the oldest go (the next one's newline with it)
    NSUInteger cut = _lengths.front() + (_lengths.size() > 1 ? 1 : 0);
    _lengths.pop_front();
    if (!_lengths.empty()) _lengths.front() -= 1;
    [_list.textStorage deleteCharactersInRange:NSMakeRange(0, std::min<NSUInteger>(cut, _list.textStorage.length))];
  }
  _empty.hidden = YES;
  if (atEnd || m["mine"].b(false)) [_list scrollToEndOfDocument:nil];
}

- (void)note:(NSString*)text lasting:(BOOL)lasting {
  [_noteTimer invalidate];
  _noteTimer = nil;
  _note.stringValue = text ?: @"";
  _note.hidden = text.length == 0;
  if (!lasting && text.length) {
    __weak SSChatView* weak = self;
    _noteTimer = [NSTimer scheduledTimerWithTimeInterval:6 repeats:NO block:^(NSTimer*) { [weak note:nil lasting:NO]; }];
  }
}

- (void)serverName:(NSString*)name {
  _name.placeholderString = name.length ? name : @"a neved a chatben";
}

- (void)restore:(NSString*)text {
  if (text.length && !_input.stringValue.length) _input.stringValue = text;
}

- (void)clear {
  [_list.textStorage setAttributedString:[NSAttributedString new]];
  _lengths.clear();
  _empty.hidden = NO;
  [self note:nil lasting:NO];
  _name.stringValue = [NSUserDefaults.standardUserDefaults stringForKey:@"chatName"] ?: @""; // (maybe changed in another window)
}

- (void)focusInput {
  [self.window makeFirstResponder:_input];
}

- (NSString*)shown { // (CI: what the list shows)
  return _list.textStorage.string;
}
@end

/* ------------------------------------------------ the app */

@class SSWin;

/** The app: the menus, the About panel, updates – and its windows: any number, each sharing or
 *  watching on its own (more rooms watched at once). */
@interface SSApp : NSObject <NSApplicationDelegate>
@property(nonatomic, readonly) NSArray<SSWin*>* windows;
- (void)newWindowFrom:(SSWin*)from watch:(BOOL)watch;
- (void)windowClosed:(SSWin*)w;
- (void)installUpdate;
- (void)openWindows;
@end

/** One window: home, sharing, watching. */
@interface SSWin : NSObject <NSWindowDelegate, NSTextFieldDelegate>
@property(nonatomic, readonly) NSWindow* window;
- (instancetype)initFrom:(SSWin*)from watch:(BOOL)watch;
- (NSEvent*)key:(NSEvent*)e;
- (void)endSession;
- (bool)busy;
- (void)ask:(NSString*)title text:(NSString*)text ok:(NSString*)ok cancel:(NSString*)cancel danger:(BOOL)danger action:(void (^)(void))action;
- (void)ask:(NSString*)title text:(NSString*)text ok:(NSString*)ok cancel:(NSString*)cancel danger:(BOOL)danger action:(void (^)(void))action otherwise:(void (^)(void))otherwise;
- (void)showUpdate:(NSString*)version;
- (void)updateBusy:(BOOL)busy failed:(BOOL)failed;
// the room kept (the app goes back into it when it starts again)
- (void)restore:(const SavedRoom&)room;
- (bool)holds:(const std::string&)code;
- (bool)sharing;
- (void)goAway:(bool)all;
- (void)leave;
- (std::string)describe; // (CI: what the window shows)
// CI (--ui-chat): watch on this server, say something in the chat, what the chat shows
- (void)testWatch:(const std::string&)code server:(const std::string&)server;
- (void)testSay:(NSString*)text;
- (NSString*)chatShown;
- (NSView*)chatPanel;
- (bool)chatReady;
@end

@implementation SSWin {
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
  // the room's chat: the sharer's box, the viewer's panel at the right of the picture (in full screen
  // too); closed, a count on its button and the newest message over the picture for a few seconds
  SSChatView *_hostChat, *_viewChat;
  NSView *_chatPanel, *_chatBadge, *_pop;
  NSTextField *_chatBadgeText, *_popText;
  NSLayoutConstraint* _chatShift;
  bool _chatOpen;
  int _unread;
  NSString* _chatPending;
  NSTimer* _popTimer;
  bool _chatReady; // in the room (the server told our name)
  std::string _server; // (CI: a local one)
  // the room this window is in, kept in the app's settings while it is (see -[SSApp restoreRooms:])
  std::string _savedCode, _savedToken;
  bool _savedHost;
  double _savedAt;
  bool _restoring; // this session goes back into a kept room (gone: home, with a note)
  bool _keepRoom;  // the window closes but its room stays (the server keeps it 10 minutes)

  std::unique_ptr<Engine> _engine;
  std::unique_ptr<MacHost> _hostPipe;
  std::unique_ptr<MacPlayer> _player;
  std::unique_ptr<MacAudioOut> _audioOut;
  int _session;
  bool _live, _picture, _stats, _muted, _barHidden;
  std::string _roomLink, _statusKind;
  Json _viewStats;
  NSString *_savedPassword, *_passwordToSave;
  NSTimer *_tickTimer, *_barTimer;
}

- (instancetype)initFrom:(SSWin*)from watch:(BOOL)watch {
  if (!(self = [super init])) return nil;
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kHomeWidth + 2 * kSide, 560)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskFullSizeContentView
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.releasedWhenClosed = NO; // (this object keeps it)
  _window.title = @"Screen Share";
  _window.titlebarAppearsTransparent = YES;
  _window.titleVisibility = NSWindowTitleHidden;
  _window.movableByWindowBackground = YES;
  _window.delegate = self;
  _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
  _window.tabbingMode = NSWindowTabbingModeDisallowed; // (windows side by side, not tabs)
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
  if (const char* e = getenv("SS_SERVER")) _server = e; // (tests, troubleshooting: another server)
  [self makeHome];
  [self makeHost];
  [self makeViewer];
  if (watch) { // (another window: to watch one more room)
    _tabs.selectedSegment = 1;
    [self applyTab];
  }
  [self show:_home];
  [self fitWindow:NO];
  if (from) { // a little lower and to the right of the window it came from
    NSRect f = from.window.frame;
    [_window setFrameTopLeftPoint:NSMakePoint(NSMinX(f) + 26, NSMaxY(f) - 26)];
    [_window setFrame:[self clamp:_window.frame] display:NO];
  } else {
    [_window center];
  }
  [_window makeKeyAndOrderFront:nil];
  if (watch) [_window makeFirstResponder:_code];
  __weak SSWin* weak = self;
  _tickTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer*) { [weak tick]; }];
  _barTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) { [weak barTick]; }];
  return self;
}

- (void)dealloc {
  _window.delegate = nil;
}

- (bool)busy {
  return _engine != nullptr;
}

- (NSEvent*)key:(NSEvent*)e {
  if (e.modifierFlags & NSEventModifierFlagCommand) return e;
  if (e.window != _window || _window.attachedSheet || _current != _viewer) return e;
  if ([_window.firstResponder isKindOfClass:NSText.class]) return e; // (typing in the chat)
  NSString* k = e.charactersIgnoringModifiers.lowercaseString;
  if ([k isEqualToString:@"c"]) [self setChatOpen:!_chatOpen focus:YES];
  else if ([k isEqualToString:@"f"]) [_window toggleFullScreen:nil];
  else if ([k isEqualToString:@"s"]) {
    _stats = !_stats;
    [self tick];
  } else if ([k isEqualToString:@"m"]) [self setMuted:!_muted];
  else return e;
  [self moved];
  return nil;
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
    if (r.size.width >= 640 && r.size.height >= 400 && NSIntersectsRect(r, vis)) return [self besideOthers:r];
  }
  CGFloat w = std::min<CGFloat>(1280, vis.size.width * 0.86), h = w * 9 / 16;
  if (h > vis.size.height * 0.86) {
    h = vis.size.height * 0.86;
    w = h * 16 / 9;
  }
  NSRect old = _window.frame;
  return [self besideOthers:NSMakeRect(NSMidX(old) - w / 2, NSMidY(old) - h / 2, w, h)];
}

/** Not right on top of another window watching there: a little lower and to the right. */
- (NSRect)besideOthers:(NSRect)r {
  for (int i = 0; i < 12; i++) {
    bool taken = false;
    for (SSWin* w in ((SSApp*)NSApp.delegate).windows)
      if (w != self && std::fabs(NSMinX(w.window.frame) - NSMinX(r)) < 4 && std::fabs(NSMaxY(w.window.frame) - NSMaxY(r)) < 4) taken = true;
    if (!taken) break;
    r.origin.x += 26;
    r.origin.y -= 26;
  }
  return r;
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
  [self ask:title text:text ok:ok cancel:cancel danger:danger action:action otherwise:nil];
}

- (void)ask:(NSString*)title text:(NSString*)text ok:(NSString*)ok cancel:(NSString*)cancel danger:(BOOL)danger action:(void (^)(void))action otherwise:(void (^)(void))otherwise {
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
              if (r == NSAlertFirstButtonReturn) {
                if (action) action();
              } else if (otherwise) {
                otherwise();
              }
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
  dropOldPassword();
  _savedPassword = loadPassword();
  _password.stringValue = _savedPassword;
  sizeTo(_password, 210, 0);
  _remember = makeSwitch([ud objectForKey:@"rememberPassword"] ? [ud boolForKey:@"rememberPassword"] : YES);
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

  _updateButton = button(@"", @"arrow.down.circle", self, @selector(installUpdateClicked));
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
  // the chat with the viewers
  NSStackView* chatHead = hstack(@[ label(@"Chat", 13, NSFontWeightSemibold, NSColor.labelColor), spacer() ], 8);
  chatHead.edgeInsets = NSEdgeInsetsMake(0, 4, 0, 4);
  _hostChat = [self chatViewWithEmpty:@"Itt beszélgethetsz a nézőkkel – ők is írhatnak."];
  [_hostChat.heightAnchor constraintEqualToConstant:290].active = YES;
  SSGroup* chatGroup = [SSGroup new];
  [chatGroup setRows:@[ _hostChat ]];
  _hostContent = vstack(@[ _livePill, caption, _roomCode, _link, _copyButton, status, chatHead, chatGroup, viewersHead, _viewersGroup, buttons ], 4);
  [_hostContent setCustomSpacing:7 afterView:chatHead];
  [_hostContent setCustomSpacing:22 afterView:chatGroup];
  stretch(chatHead, _hostContent);
  stretch(chatGroup, _hostContent);
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
  __weak SSWin* weak = self;
  _viewer = [SSFlipped new];
  _viewer.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]; // over the picture: always dark
  _video = [[SSVideoView alloc] initWithFrame:NSZeroRect];
  _video.onDoubleClick = ^{ [weak fullScreen]; };
  _video.onMouseMove = ^{ [weak moved]; };
  [_viewer addSubview:_video];
  _video.translatesAutoresizingMaskIntoConstraints = NO;
  // the chat panel at the right (pushed out of the window when closed); the picture beside it
  _viewChat = [self chatViewWithEmpty:@"Még senki nem írt. Köszönj a többieknek!"];
  NSView* panel = [NSView new];
  panel.translatesAutoresizingMaskIntoConstraints = NO;
  panel.wantsLayer = YES;
  panel.layer.backgroundColor = [NSColor colorWithWhite:0.115 alpha:1].CGColor;
  NSView* edge = [NSView new];
  edge.translatesAutoresizingMaskIntoConstraints = NO;
  edge.wantsLayer = YES;
  edge.layer.backgroundColor = [NSColor colorWithWhite:1 alpha:0.09].CGColor;
  NSButton* close = iconButton(@"xmark", self, @selector(closeChat));
  close.toolTip = @"Chat bezárása (C)";
  NSStackView* head = hstack(@[ label(@"Chat", 13, NSFontWeightSemibold, NSColor.labelColor), spacer(), close ], 8);
  head.edgeInsets = NSEdgeInsetsMake(0, 16, 0, 10);
  for (NSView* v in @[ edge, head, _viewChat ]) [panel addSubview:v];
  [_viewer addSubview:panel];
  _chatPanel = panel;
  _chatOpen = ![NSUserDefaults.standardUserDefaults objectForKey:@"chatOpen"] || [NSUserDefaults.standardUserDefaults boolForKey:@"chatOpen"];
  _chatShift = [panel.trailingAnchor constraintEqualToAnchor:_viewer.trailingAnchor constant:_chatOpen ? 0 : 300];
  panel.hidden = !_chatOpen;
  [NSLayoutConstraint activateConstraints:@[
    [_video.leadingAnchor constraintEqualToAnchor:_viewer.leadingAnchor], [_video.topAnchor constraintEqualToAnchor:_viewer.topAnchor],
    [_video.bottomAnchor constraintEqualToAnchor:_viewer.bottomAnchor], [_video.trailingAnchor constraintEqualToAnchor:panel.leadingAnchor],
    [panel.topAnchor constraintEqualToAnchor:_viewer.topAnchor], [panel.bottomAnchor constraintEqualToAnchor:_viewer.bottomAnchor],
    [panel.widthAnchor constraintEqualToConstant:300], _chatShift,
    [edge.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor], [edge.topAnchor constraintEqualToAnchor:panel.topAnchor],
    [edge.bottomAnchor constraintEqualToAnchor:panel.bottomAnchor], [edge.widthAnchor constraintEqualToConstant:1],
    [head.topAnchor constraintEqualToAnchor:panel.topAnchor constant:30], [head.heightAnchor constraintEqualToConstant:34],
    [head.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor], [head.trailingAnchor constraintEqualToAnchor:panel.trailingAnchor],
    [_viewChat.topAnchor constraintEqualToAnchor:head.bottomAnchor constant:4], [_viewChat.bottomAnchor constraintEqualToAnchor:panel.bottomAnchor],
    [_viewChat.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor], [_viewChat.trailingAnchor constraintEqualToAnchor:panel.trailingAnchor]
  ]];
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
    [_overlay.centerXAnchor constraintEqualToAnchor:_video.centerXAnchor], [_overlay.centerYAnchor constraintEqualToAnchor:_viewer.centerYAnchor]
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
  full.toolTip = @"Teljes képernyő (F)";
  _muteButton.toolTip = @"Némítás (M)";
  NSButton* more = iconButton(@"plus.rectangle.on.rectangle", self, @selector(newWindow));
  more.toolTip = @"Új ablak – még egy szoba nézése (⌘N)";
  NSButton* leave = button(@"Kilépés", nil, self, @selector(stopToHome));
  leave.hasDestructiveAction = YES;
  // the chat's button, with the count of what came while the panel was closed
  NSButton* chat = iconButton(@"bubble.left.and.bubble.right", self, @selector(toggleChat));
  chat.toolTip = @"Chat (C)";
  NSView* chatBox = [NSView new];
  chatBox.translatesAutoresizingMaskIntoConstraints = NO;
  [chatBox addSubview:chat];
  pin(chat, chatBox);
  _chatBadgeText = label(@"", 10, NSFontWeightBold, NSColor.whiteColor);
  _chatBadgeText.alignment = NSTextAlignmentCenter;
  _chatBadge = [NSView new];
  _chatBadge.translatesAutoresizingMaskIntoConstraints = NO;
  _chatBadge.wantsLayer = YES;
  _chatBadge.layer.backgroundColor = chatAccent().CGColor;
  _chatBadge.layer.cornerRadius = 8;
  _chatBadge.hidden = YES;
  [_chatBadge addSubview:_chatBadgeText];
  [chatBox addSubview:_chatBadge];
  [NSLayoutConstraint activateConstraints:@[
    [_chatBadge.heightAnchor constraintEqualToConstant:16], [_chatBadge.widthAnchor constraintGreaterThanOrEqualToConstant:16],
    [_chatBadge.widthAnchor constraintGreaterThanOrEqualToAnchor:_chatBadgeText.widthAnchor constant:8],
    [_chatBadge.centerXAnchor constraintEqualToAnchor:chat.trailingAnchor constant:-2], [_chatBadge.centerYAnchor constraintEqualToAnchor:chat.topAnchor constant:3],
    [_chatBadgeText.centerXAnchor constraintEqualToAnchor:_chatBadge.centerXAnchor], [_chatBadgeText.centerYAnchor constraintEqualToAnchor:_chatBadge.centerYAnchor]
  ]];
  NSStackView* row = hstack(@[ _viewDot, _viewTitle, _viewInfo, chatBox, more, _muteButton, _volume, full, leave ], 10);
  // a narrow picture (the chat beside it): the less needed ones step aside first
  [row setVisibilityPriority:600 forView:_viewInfo];
  [row setVisibilityPriority:700 forView:_volume];
  [row setVisibilityPriority:800 forView:more];
  [row setVisibilityPriority:900 forView:_viewTitle];
  [row setCustomSpacing:14 afterView:_viewInfo];
  [row setCustomSpacing:4 afterView:_muteButton];
  [row setCustomSpacing:14 afterView:full];
  row.edgeInsets = NSEdgeInsetsMake(0, 20, 0, 9);
  _bar = glassCapsule(row, 25);
  [_viewer addSubview:_bar];
  [NSLayoutConstraint activateConstraints:@[
    [_bar.heightAnchor constraintEqualToConstant:50], [_bar.centerXAnchor constraintEqualToAnchor:_video.centerXAnchor],
    [_bar.bottomAnchor constraintEqualToAnchor:_viewer.bottomAnchor constant:-22],
    [_bar.widthAnchor constraintLessThanOrEqualToAnchor:_video.widthAnchor constant:-40]
  ]];
  // with the panel closed, the newest message over the picture for a few seconds (a click opens the chat)
  _popText = label(@"", 13, NSFontWeightRegular, NSColor.labelColor, 400);
  NSStackView* popRow = hstack(@[ _popText ], 0);
  popRow.edgeInsets = NSEdgeInsetsMake(9, 14, 9, 14);
  _pop = glassCapsule(popRow, 14);
  _pop.alphaValue = 0;
  [_pop addGestureRecognizer:[[NSClickGestureRecognizer alloc] initWithTarget:self action:@selector(openChat)]];
  [_viewer addSubview:_pop];
  [NSLayoutConstraint activateConstraints:@[
    [_pop.leadingAnchor constraintEqualToAnchor:_video.leadingAnchor constant:22], [_pop.bottomAnchor constraintEqualToAnchor:_bar.topAnchor constant:-12],
    [_pop.widthAnchor constraintLessThanOrEqualToConstant:440]
  ]];
}

/* ---- the room's chat */

- (SSChatView*)chatViewWithEmpty:(NSString*)empty {
  SSChatView* chat = [[SSChatView alloc] initWithEmpty:empty];
  __weak SSWin* weak = self;
  chat.onSend = ^(NSString* text) { [weak sendChat:text]; };
  chat.onName = ^(NSString* name) { [weak nameChanged:name]; };
  return chat;
}

/** The chat of what this window shows now. */
- (SSChatView*)chat {
  return _current == _viewer ? _viewChat : _hostChat;
}

- (void)sendChat:(NSString*)text {
  if (!_engine) {
    [[self chat] restore:text];
    [[self chat] note:@"Nem vagy szobában – az üzenet nem ment el." lasting:NO];
    return;
  }
  _chatPending = text; // (back in the box if it could not go)
  _engine->sendChat(U(text));
}

- (void)nameChanged:(NSString*)name {
  if (_engine && name.length) _engine->setName(U(name));
}

- (void)setChatOpen:(bool)open focus:(bool)focus {
  _chatOpen = open;
  [NSUserDefaults.standardUserDefaults setBool:open forKey:@"chatOpen"];
  if (open) {
    _unread = 0;
    [self updateUnread];
    [self hidePop];
    _chatPanel.hidden = NO;
  }
  NSView* panel = _chatPanel;
  [NSAnimationContext
      runAnimationGroup:^(NSAnimationContext* ctx) {
        ctx.duration = 0.22;
        ctx.allowsImplicitAnimation = YES;
        self->_chatShift.animator.constant = open ? 0 : 300;
        [self->_viewer layoutSubtreeIfNeeded];
      }
      completionHandler:^{
        if (!self->_chatOpen) panel.hidden = YES;
      }];
  if (open && focus) [_viewChat focusInput];
  else if (!open) [_window makeFirstResponder:_window.contentView];
}

- (void)toggleChat {
  [self setChatOpen:!_chatOpen focus:YES];
}

- (void)openChat {
  [self setChatOpen:true focus:YES];
}

- (void)closeChat {
  [self setChatOpen:false focus:NO];
}

- (void)updateUnread {
  _chatBadge.hidden = _unread == 0 || _chatOpen;
  _chatBadgeText.stringValue = _unread > 99 ? @"99+" : [NSString stringWithFormat:@"%d", _unread];
}

- (void)pop:(const Json&)m {
  NSMutableAttributedString* s = [NSMutableAttributedString new];
  NSColor* color = m["host"].b(false) ? chatAccent() : nameColor(m["from"].str());
  [s appendAttributedString:[[NSAttributedString alloc] initWithString:[S(m["name"].str()) stringByAppendingString:@": "]
                                                            attributes:@{NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold], NSForegroundColorAttributeName : color}]];
  [s appendAttributedString:[[NSAttributedString alloc] initWithString:S(m["text"].str())
                                                            attributes:@{NSFontAttributeName : [NSFont systemFontOfSize:13], NSForegroundColorAttributeName : NSColor.labelColor}]];
  _popText.attributedStringValue = s;
  animateAlpha(_pop, 1, 0.15);
  [_popTimer invalidate];
  __weak SSWin* weak = self;
  _popTimer = [NSTimer scheduledTimerWithTimeInterval:6 repeats:NO block:^(NSTimer*) { [weak hidePop]; }];
}

- (void)hidePop {
  [_popTimer invalidate];
  _popTimer = nil;
  if (_pop.alphaValue > 0) animateAlpha(_pop, 0, 0.5);
}

- (void)testWatch:(const std::string&)code server:(const std::string&)server {
  _server = server;
  [self watch:code];
}

- (void)testSay:(NSString*)text {
  [[self chat] typeAndSend:text];
}

- (NSString*)chatShown {
  return [[self chat] shown];
}

- (NSView*)chatPanel {
  return _chatPanel;
}

- (bool)chatReady {
  return _chatReady;
}

/** A new room: its chat starts empty (what was said there comes from the server); our name goes with it. */
- (void)startChat {
  [_hostChat clear];
  [_viewChat clear];
  _unread = 0;
  _chatPending = nil;
  _chatReady = false;
  [self updateUnread];
  [self hidePop];
  if (_engine) _engine->setName(U([NSUserDefaults.standardUserDefaults stringForKey:@"chatName"] ?: @""));
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
  __weak SSWin* weak = self;
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
  [self hidePop];
  if (_player) _player->detach(); // its decoder thread calls the engine no more
  if (_hostPipe) _hostPipe->stop();
  _engine.reset();
  _hostPipe.reset();
  _player.reset();
  _audioOut.reset();
  _picture = false;
}

- (void)share {
  [self share:"" token:""];
}

/** Sharing; `resumeCode` / `resumeToken`: going back into that room (kept by the server for its sharer:
 *  the same code and link – the token does, no password needed). */
- (void)share:(const std::string&)resumeCode token:(const std::string&)resumeToken {
  NSString* pw = _password.stringValue;
  bool resume = !resumeCode.empty();
  if (!pw.length && !resume) {
    [self setError:_shareError text:@"Add meg a megosztási jelszót."];
    [_window makeFirstResponder:_password];
    return;
  }
  if (_displays.empty()) {
    if (resume) [self forgetSaved];
    [self setError:_shareError text:@"Nem találok képernyőt."];
    return;
  }
  NSInteger quality = std::max<NSInteger>(_qualityPopup.indexOfSelectedItem, 0);
  NSInteger monitor = std::max<NSInteger>(_displayPopup.indexOfSelectedItem, 0);
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  [ud setInteger:quality forKey:@"quality"];
  [ud setBool:_audio.state == NSControlStateValueOn forKey:@"audio"];
  [ud setBool:_remember.state == NSControlStateValueOn forKey:@"rememberPassword"];
  // into the keychain once the server took it (a mistyped one is not remembered), only when it changes
  // (going back into a room: the token let us in, the password field says nothing)
  _passwordToSave = resume ? nil : _remember.state == NSControlStateValueOn ? pw : @"";
  if (_passwordToSave && !_passwordToSave.length) [self savePasswordNow];
  _shareError.hidden = YES;
  [self endSession];
  int session = ++_session;
  EngineConfig cfg;
  cfg.server = _server.empty() ? kServer : _server;
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
  _window.title = @"Megosztás";
  [self show:_host];
  [self startChat];
  _engine->host(U(pw), resumeCode, resumeToken);
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
  cfg.server = _server.empty() ? kServer : _server;
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
  _window.title = S("Szoba " + code); // (the Window menu, Mission Control: which room this window shows)
  _viewInfo.stringValue = @"";
  setDotColor(_viewDot, NSColor.systemOrangeColor);
  [self show:_viewer];
  [self startChat];
  engine->join(code);
}

- (void)stopToHome {
  [self forgetSaved];
  [self endSession];
  _window.title = @"Screen Share";
  [_overlaySpinner stopAnimation:nil];
  if (_window.styleMask & NSWindowStyleMaskFullScreen) {
    _homeAfterFullScreen = true; // the window first leaves full screen, then shrinks
    [_window toggleFullScreen:nil];
    return;
  }
  [self show:_home];
}

/* ---- the room kept: the app goes back into it */

/** This window's room into the settings – and again now and then (how fresh it is says whether the
 *  server still keeps it). */
- (void)rememberRoom:(const std::string&)code host:(bool)host token:(const std::string&)token {
  if (!_savedCode.empty() && _savedCode != code) forgetRoom(_savedCode);
  _savedCode = code;
  _savedHost = host;
  _savedToken = token;
  _savedAt = nowMs();
  saveRoom(code, host, token);
}

/** Done with the room (ended, left): the app does not go back into it. */
- (void)forgetSaved {
  forgetRoom(_savedCode);
  _savedCode.clear();
  _savedToken.clear();
  _restoring = false;
}

/** Back into a kept room: sharing it again (with its token – the same code and link), or watching it. */
- (void)restore:(const SavedRoom&)r {
  _restoring = true;
  _savedCode = r.code; // (gone, it is forgotten)
  _savedHost = r.host;
  _savedToken = r.token;
  _savedAt = nowMs();
  saveRoom(r.code, r.host, r.token); // (this window has it now)
  if (r.host) [self share:r.code token:r.token];
  else [self watch:r.code];
}

- (bool)holds:(const std::string&)code {
  return _savedCode == code;
}

- (bool)sharing {
  return _current == _host && _engine;
}

/** The app quits: the sharing's room is kept (the server waits 10 minutes; the app goes back into it
 *  when it starts again), a watched one too when `all` (the Mac restarting, an update) – else it is left. */
- (void)goAway:(bool)all {
  if (_engine && !_savedCode.empty() && (all || _current == _host)) _engine->keepRoom();
  else [self forgetSaved];
  [self endSession];
}

/** (CI) Out of the room for good. */
- (void)leave {
  [self forgetSaved];
  [self endSession];
}

- (std::string)describe {
  NSTextField* note = !_joinError.hidden ? _joinError : !_shareError.hidden ? _shareError : nil;
  Json d = Json::object();
  d.set("screen", _current == _host ? "host" : _current == _viewer ? "viewer" : "home").set("room", _savedCode).set("link", _roomLink);
  d.set("picture", _picture).set("title", U(_window.title)).set("note", note ? U(note.stringValue) : "").set("tab", int(_tabs.selectedSegment));
  return d.dump();
}

- (void)askStop {
  __weak SSWin* weak = self;
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

- (void)savePasswordNow {
  NSString* keep = _passwordToSave;
  _passwordToSave = nil;
  if (!keep || [keep isEqualToString:_savedPassword]) return;
  savePassword(keep);
  _savedPassword = keep;
}

- (void)onEvent:(const Json&)e {
  const std::string& type = e["type"].str();
  if (type == "status") {
    [self setStatus:S(e["text"].str()) kind:e["kind"].str()];
  } else if (type == "hosted") {
    [self savePasswordNow];
    _roomCode.attributedStringValue = [[NSAttributedString alloc] initWithString:S(e["code"].str()) attributes:@{
      NSFontAttributeName : [NSFont monospacedSystemFontOfSize:40 weight:NSFontWeightBold],
      NSForegroundColorAttributeName : NSColor.labelColor,
      NSKernAttributeName : @5
    }];
    _roomLink = e["link"].str();
    _link.stringValue = S(_roomLink);
    _window.title = [@"Megosztás · " stringByAppendingString:S(e["code"].str())];
    _copyButton.enabled = YES;
    [self rememberRoom:e["code"].str() host:true token:e["token"].str()];
    _restoring = false;
    if (e["changed"].b())
      [self ask:@"Új link készült" text:@"A korábbi szoba lejárt, ezért új kód és link készült – küldd el újra a nézőknek." ok:@"Rendben" cancel:nil danger:NO action:nil];
  } else if (type == "host-error") {
    std::string c = e["code"].str();
    NSString* text = c == "auth" ? @"Hibás megosztási jelszó." : c == "rate" ? @"Túl sok próbálkozás, várj egy kicsit." : c == "full" ? @"A szerver megtelt, próbáld később."
                                                                                                                                      : S("Nem érem el a szervert (" + c + ").");
    if (_restoring && c == "auth") text = @"A korábbi szoba közben bezárult – indítsd újra a megosztást.";
    [self forgetSaved];
    [self endSession];
    _window.title = @"Screen Share";
    [self show:_home];
    [self setError:_shareError text:text];
  } else if (type == "viewers") {
    [self updateViewers:e["list"]];
  } else if (type == "closed" && _current == _host) {
    [self stopToHome];
    [self setError:_shareError text:@"A szobát egy másik ablak vette át."];
  } else if (type == "closed") {
    [self forgetSaved]; // (the sharer ended it)
  } else if (type == "joined") {
    [self rememberRoom:e["code"].str() host:false token:""];
    _restoring = false;
  } else if (type == "join-error" && _restoring && e["code"].str() == "notfound") {
    std::string code = _savedCode;
    [self stopToHome];
    _tabs.selectedSegment = 1; // (the note is on the Nézés tab)
    [self applyTab];
    [self setError:_joinError text:S("A szoba (" + code + "), amit néztél, közben bezárult.")];
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
  } else if (type == "chat-history") {
    [[self chat] history:e["list"]];
    if (!e["supported"].b(true)) [[self chat] note:@"A szerver még nem tud chatet – frissíteni kell (sudo bash deploy/install.sh)." lasting:YES];
  } else if (type == "chat") {
    [[self chat] add:e];
    if (e["mine"].b(false)) {
      _chatPending = nil;
    } else {
      if (_current == _viewer && !_chatOpen) {
        _unread++;
        [self updateUnread];
        [self pop:e];
      }
      if (!NSApp.active) [NSApp requestUserAttention:NSInformationalRequest]; // (the Dock icon bounces once)
    }
  } else if (type == "chat-limit") {
    [[self chat] note:@"Lassabban – túl sok üzenet egyszerre. Pár másodperc múlva mehet." lasting:NO];
  } else if (type == "chat-offline") {
    [[self chat] restore:_chatPending];
    _chatPending = nil;
    [[self chat] note:@"Nincs kapcsolat a szobával – az üzenet nem ment el." lasting:NO];
  } else if (type == "name") {
    [[self chat] serverName:S(e["name"].str())];
    _chatReady = true;
  }
}

- (void)tick {
  if (!_savedCode.empty() && nowMs() - _savedAt > 20000) [self rememberRoom:_savedCode host:_savedHost token:_savedToken];
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

/* ---- window */

- (BOOL)windowShouldClose:(NSWindow*)w {
  if (_current != _host) return YES;
  __weak NSWindow* win = w;
  __weak SSWin* weak = self;
  [self ask:@"Bezárod?" text:kKeptText ok:@"Bezárás" cancel:@"Mégse" danger:NO action:^{
    SSWin* s = weak;
    if (s) s->_keepRoom = true;
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

- (void)windowWillClose:(NSNotification*)n {
  if (_keepRoom && _engine) _engine->keepRoom(); // (the sharing's room waits for the app: see windowShouldClose)
  else [self forgetSaved];                      // (a viewer's window closed: leaving the room)
  [self endSession];
  [_tickTimer invalidate];
  [_barTimer invalidate];
  [(SSApp*)NSApp.delegate windowClosed:self];
}

/* ---- more windows */

- (void)newWindow {
  [(SSApp*)NSApp.delegate newWindowFrom:self watch:YES];
}

/* ---- updates (the app finds and installs them; each window shows the button) */

- (void)showUpdate:(NSString*)version {
  _updateButton.title = [NSString stringWithFormat:@"Frissítés: %@", version];
  _updateButton.enabled = YES;
  if (_updateButton.hidden) {
    _updateButton.alphaValue = 0;
    _updateButton.hidden = NO;
    animateAlpha(_updateButton, 1, 0.3);
    [self fitWindow:YES];
  }
}

- (void)updateBusy:(BOOL)busy failed:(BOOL)failed {
  if (busy) _updateButton.title = @"Letöltés…";
  else if (failed) _updateButton.title = @"Frissítés sikertelen – újra";
  _updateButton.enabled = !busy;
}

- (void)installUpdateClicked {
  [(SSApp*)NSApp.delegate installUpdate];
}

@end

/* ------------------------------------------------ the app: menus, About, updates, the windows */

@implementation SSApp {
  NSMutableArray<SSWin*>* _windows;
  NSString *_updateFile, *_updateVersion;
  bool _checking;
  bool _keepRooms; // the app goes away for a while (an update, the Mac restarting): every room stays
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
  if ([self signLocallyFirst]) return;
  [self openWindows];
  [NSApp activateIgnoringOtherApps:YES];
  [self checkUpdatesLater];
  __weak SSApp* weak = self;
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                        handler:^NSEvent*(NSEvent* e) {
                                          SSApp* s = weak;
                                          if (s)
                                            for (SSWin* w in [s->_windows copy])
                                              if (w.window == e.window) return [w key:e];
                                          return e;
                                        }];
}

- (NSArray<SSWin*>*)windows {
  return _windows;
}

/** The first window – and back into the rooms the app was in when it went away. */
- (void)openWindows {
  _windows = [NSMutableArray array];
  [self makeMenu];
  [self newWindowFrom:nil watch:NO];
  [self restoreRooms:_windows.firstObject];
}

/** The rooms this app was in when it went away – a crash, an update, the Mac restarting, the sharer's
 *  window closed (each kept 10 minutes by the server): back into them, the sharing in `first` (a window
 *  on its home screen), each room watched in a window of its own. Not the ones a window is in. */
- (void)restoreRooms:(SSWin*)first {
  double now = NSDate.date.timeIntervalSince1970;
  std::vector<SavedRoom> todo;
  for (const SavedRoom& r : savedRooms()) {
    if (now - r.at > kRoomKeepS || r.code.size() < 4) {
      forgetRoom(r.code);
      continue;
    }
    bool held = otherCopy(r.pid);
    for (SSWin* w in _windows) held = held || [w holds:r.code];
    if (!held) todo.push_back(r);
  }
  std::stable_sort(todo.begin(), todo.end(), [](const SavedRoom& a, const SavedRoom& b) { return a.host > b.host; });
  bool shared = false;
  for (SSWin* w in _windows) shared = shared || [w sharing];
  for (const SavedRoom& r : todo) {
    if (r.host && shared) { // (one sharing at a time)
      forgetRoom(r.code);
      continue;
    }
    shared = shared || r.host;
    SSWin* w = first && ![first busy] ? first : nil;
    first = nil;
    if (!w) {
      [self newWindowFrom:[self front] watch:YES];
      w = _windows.lastObject;
    }
    [w restore:r];
  }
}

/** Another window – to watch one more room: any number at once, each on its own. */
- (void)newWindowFrom:(SSWin*)from watch:(BOOL)watch {
  SSWin* w = [[SSWin alloc] initFrom:from watch:watch];
  [_windows addObject:w];
  if (_updateVersion) [w showUpdate:_updateVersion];
}

/** Fájl → Új ablak (⌘N), the Dock's menu. */
- (void)newWindow:(id)sender {
  [self newWindowFrom:[self front] watch:YES];
}

- (void)windowClosed:(SSWin*)w {
  // (once its close has finished: the window's object goes with it)
  dispatch_async(dispatch_get_main_queue(), ^{
    [self->_windows removeObject:w];
  });
}

/** The window in front (where questions go). */
- (SSWin*)front {
  for (SSWin* w in _windows)
    if (w.window.isKeyWindow) return w;
  for (SSWin* w in _windows)
    if (w.window.isMainWindow) return w;
  return _windows.lastObject;
}

- (NSMenu*)applicationDockMenu:(NSApplication*)sender {
  NSMenu* m = [NSMenu new];
  [m addItemWithTitle:@"Új ablak" action:@selector(newWindow:) keyEquivalent:@""].target = self;
  return m;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)a hasVisibleWindows:(BOOL)visible {
  if (!_windows.count) [self newWindowFrom:nil watch:NO];
  [self restoreRooms:nil]; // (started again while it runs: the sharing whose window was closed)
  return YES;
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

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a {
  return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)a {
  return YES;
}

/** ⌘Q while sharing: the room stays (as when its window closes) – said first. The Mac shutting down,
 *  restarting, logging out: every room stays, the app goes back into them when it starts again. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)a {
  if (_keepRooms || systemQuit()) {
    _keepRooms = true;
    return NSTerminateNow;
  }
  SSWin* sharing = nil;
  for (SSWin* w in _windows)
    if ([w sharing]) sharing = w;
  if (!sharing) return NSTerminateNow;
  [sharing.window makeKeyAndOrderFront:nil];
  [sharing ask:@"Kilépsz?" text:kKeptText ok:@"Kilépés" cancel:@"Mégse" danger:NO
      action:^{ [NSApp replyToApplicationShouldTerminate:YES]; }
      otherwise:^{ [NSApp replyToApplicationShouldTerminate:NO]; }];
  return NSTerminateLater;
}

- (void)applicationWillTerminate:(NSNotification*)n {
  for (SSWin* w in [_windows copy]) [w goAway:_keepRooms];
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
  NSMenuItem* fileItem = [NSMenuItem new];
  [bar addItem:fileItem];
  NSMenu* file = [[NSMenu alloc] initWithTitle:@"Fájl"];
  [file addItemWithTitle:@"Új ablak" action:@selector(newWindow:) keyEquivalent:@"n"].target = self;
  [file addItemWithTitle:@"Ablak bezárása" action:@selector(performClose:) keyEquivalent:@"w"];
  fileItem.submenu = file;
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
    for (SSWin* w in _windows) [w showUpdate:version];
  }
  if (!asked) return;
  __weak SSApp* weak = self;
  SSWin* front = [self front];
  if (version)
    [front ask:[NSString stringWithFormat:@"Elérhető a Screen Share %@", version]
         text:[NSString stringWithFormat:@"A telepített verzió: %@. A frissítés letöltődik, majd a Screen Share újraindul.", @SS_VERSION]
           ok:@"Frissítés"
       cancel:@"Később"
       danger:NO
       action:^{
         [weak installUpdate];
       }];
  else if (failed)
    [front ask:@"Nem érem el a frissítéseket" text:@"Ellenőrizd az internetkapcsolatot, és próbáld újra később." ok:@"OK" cancel:nil danger:NO action:nil];
  else
    [front ask:@"Ez a legújabb verzió" text:[NSString stringWithFormat:@"A Screen Share %@ a legfrissebb.", @SS_VERSION] ok:@"OK" cancel:nil danger:NO action:nil];
}

- (void)installUpdate {
  if (!_updateFile) return;
  // the app starts again: what the other windows show ends too – asked first
  int busy = 0;
  for (SSWin* w in _windows) busy += [w busy] ? 1 : 0;
  if (busy > ([[self front] busy] ? 1 : 0)) {
    __weak SSApp* weak = self;
    [[self front] ask:@"Frissítés"
                 text:@"A frissítéshez a Screen Share újraindul: a többi ablakban futó nézés vagy megosztás is véget ér."
                   ok:@"Frissítés"
               cancel:@"Mégse"
               danger:NO
               action:^{
                 [weak downloadUpdate];
               }];
    return;
  }
  [self downloadUpdate];
}

- (void)downloadUpdate {
  NSString* file = _updateFile;
  if (!file) return;
  for (SSWin* w in _windows) [w updateBusy:YES failed:NO];
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
                                        for (SSWin* w in self->_windows) [w updateBusy:NO failed:YES];
                                        [[self front] ask:@"A frissítés nem sikerült" text:@"Nem tudtam letölteni az új verziót. Próbáld újra később." ok:@"OK" cancel:nil danger:NO action:nil];
                                        return;
                                      }
                                      // after we quit: the new app (signed with this Mac's identity) in place of this one, then start it
                                      self->_keepRooms = true; // (the new version goes back into the rooms)
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
  cfg.forceTcp = getenv("SS_FORCE_TCP") != nullptr; // (CI: the WebSocket path, as to a browser)
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
    // SS_VIEWERS=n: n viewers at once in this one process, as n windows of the app watch
    int n = 1;
    if (const char* e = getenv("SS_VIEWERS")) n = std::max(1, std::min(16, atoi(e)));
    struct Viewer {
      std::unique_ptr<MacPlayer> player;
      std::unique_ptr<MacAudioOut> audio;
      std::unique_ptr<Engine> engine;
      std::atomic<int> frames{0}, sounds{0};
      double best = 0;
      MacPlayer::Stats st;
    };
    std::vector<std::unique_ptr<Viewer>> viewers;
    for (int k = 0; k < n; k++) {
      auto v = std::make_unique<Viewer>();
      v->player.reset(new MacPlayer([CALayer layer]));
      v->audio.reset(new MacAudioOut());
      Viewer* vp = v.get();
      EngineCallbacks each = cb;
      each.media = [vp](const MediaMeta& m, const uint8_t* d, size_t len) {
        if (m.stream == 0) {
          vp->frames++;
          vp->player->push(d, len, m.key, m.capture);
        } else {
          vp->sounds++;
          vp->audio->push(m.sub, d, len, m.ts);
        }
      };
      v->engine.reset(new Engine(cfg, each));
      Engine* engine = v->engine.get();
      v->player->clockOffset = [engine] { return engine->clockOffset(); };
      v->player->needKeyframe = [engine] { engine->requestKeyframe(); };
      engine->join(secret);
      viewers.push_back(std::move(v));
    }
    for (int t = 0; t < seconds * 10; t++) {
      runFor(0.1);
      for (auto& v : viewers) {
        v->st = v->player->stats();
        v->best = std::max(v->best, v->st.fps);
      }
    }
    for (auto& v : viewers) v->player->detach();
    const Viewer& f = *viewers[0];
    result.set("received", f.frames.load()).set("audioPackets", f.sounds.load()).set("shownFps", f.best).set("width", f.st.width).set("height", f.st.height).set("latencyMs", f.st.latencyMs);
    result.set("metal", f.st.metal);
    if (n > 1) {
      Json each = Json::array();
      int fewest = INT_MAX;
      double slowest = 1e9;
      for (auto& v : viewers) {
        each.push(Json::object().set("received", v->frames.load()).set("shownFps", v->best).set("latencyMs", v->st.latencyMs));
        fewest = std::min(fewest, v->frames.load());
        slowest = std::min(slowest, v->best);
      }
      result.set("viewers", each).set("fewestReceived", fewest).set("slowestFps", slowest);
    }
    for (auto& v : viewers) v->engine.reset(); // (before their players)
  }
  line("RESULT " + result.dump());
  fclose(log);
  return 0;
}

/** --ui-chat <server> <code> <png> <log> (CI): a window of the app watches a room and says something in
 *  its chat – it has to come back as ours, and the sharer's answer has to show there; the chat panel is
 *  saved. Each step goes to the log as it happens (a crash shows where). */
static FILE* gChatLog = nullptr;

static void chatLog(NSString* text) {
  if (!gChatLog) return;
  fprintf(gChatLog, "%s\n", text.UTF8String);
  fflush(gChatLog);
}

static int uiChat(NSArray<NSString*>* args) {
  // (args: the app, --ui-chat, then the server, the room, the picture's and the log's paths)
  NSString *server = args[2], *code = args[3], *png = args[4];
  gChatLog = fopen(args[5].fileSystemRepresentation, "w");
  NSSetUncaughtExceptionHandler([](NSException* e) { chatLog([NSString stringWithFormat:@"EXCEPTION %@: %@\n%@", e.name, e.reason, e.callStackSymbols]); });
  NSApplication* app = NSApplication.sharedApplication;
  app.activationPolicy = NSApplicationActivationPolicyRegular;
  [app finishLaunching];
  auto runFor = [](double s) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:s];
    while ([until timeIntervalSinceNow] > 0) [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
  };
  chatLog(@"window");
  SSWin* w = [[SSWin alloc] initFrom:nil watch:YES];
  chatLog([NSString stringWithFormat:@"watching %@ on %@", code, server]);
  [w testWatch:U(code) server:U(server)];
  double started = nowMs();
  for (int i = 0; i < 300 && ![w chatReady]; i++) runFor(0.05);
  chatLog([NSString stringWithFormat:@"in the room: %s after %.1f s; the chat shows: %@", [w chatReady] ? "yes" : "no", (nowMs() - started) / 1000, [w chatShown]]);
  runFor(1);
  NSString* said = @"Szia a Mac appból! 👋";
  [w testSay:said];
  chatLog(@"said");
  bool came = false, answered = false;
  for (int i = 0; i < 100 && !(came && answered); i++) {
    runFor(0.1);
    NSString* shown = [w chatShown];
    came = [shown containsString:said];
    answered = [shown containsString:@"Szia, "]; // (the sharer answers everyone: "Szia, <name>!")
  }
  chatLog([NSString stringWithFormat:@"RESULT {\"came\":%s,\"answered\":%s}\n%@", came ? "true" : "false", answered ? "true" : "false", [w chatShown]]);
  runFor(0.5);
  @try {
    NSView* v = [w chatPanel];
    NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:png atomically:YES];
    chatLog(@"saved");
  } @catch (NSException* e) {
    chatLog([NSString stringWithFormat:@"not saved: %@", e.reason]);
  }
  [w leave];
  chatLog(@"end");
  fclose(gChatLog);
  gChatLog = nullptr;
  return came && answered ? 0 : 1;
}

/** --ui-restore <seconds> <log> (CI): the app's windows as when it starts – back into the rooms its
 *  settings keep (the test put them there) – then what each window shows, and what the settings keep. */
static int uiRestore(NSArray<NSString*>* args) {
  double seconds = args[2].doubleValue;
  FILE* log = fopen(args[3].fileSystemRepresentation, "w");
  if (!log) return 1;
  NSApplication* app = NSApplication.sharedApplication;
  app.activationPolicy = NSApplicationActivationPolicyRegular;
  [app finishLaunching];
  SSApp* delegate = [SSApp new];
  app.delegate = delegate; // (after finishLaunching: no signing, no update check)
  [delegate openWindows];
  NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
  while ([until timeIntervalSinceNow] > 0) [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
  for (SSWin* w in delegate.windows) fprintf(log, "WINDOW %s\n", [w describe].c_str());
  NSDictionary* rooms = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"rooms"] ?: @{};
  NSData* json = [NSJSONSerialization dataWithJSONObject:rooms options:0 error:nil];
  fprintf(log, "ROOMS %s\nPID %d\n", json ? [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String : "{}", getpid());
  for (SSWin* w in delegate.windows) [w leave];
  fclose(log);
  return 0;
}

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    netInit();
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    if (args.count == 6 && [args[1] isEqualToString:@"--ui-chat"]) return uiChat(args);
    if (args.count == 4 && [args[1] isEqualToString:@"--ui-restore"]) return uiRestore(args);
    if (args.count == 6 && ([args[1] isEqualToString:@"--smoke-host"] || [args[1] isEqualToString:@"--smoke-view"])) return smoke(args);
    if (args.count == 4 && [args[1] isEqualToString:@"--ws-bench"]) { // CI: round trips through this WebSocket
      FILE* log = fopen(args[3].fileSystemRepresentation, "w");
      if (!log) return 1;
      fprintf(log, "BENCH %s\n", wsBench(openMacWebSocket, U(args[2]), 240).dump().c_str());
      fclose(log);
      return 0;
    }
    if (args.count >= 3 && [args[1] isEqualToString:@"--keychain"]) { // CI: the saved password, from any build
      if ([args[2] isEqualToString:@"save"] && args.count == 4) {
        NSString* why = @"";
        if (!savePassword(args[3], &why)) {
          fprintf(stderr, "not saved: %s\n", why.UTF8String);
          return 1;
        }
      } else {
        dropOldPassword(); // (as the app starts)
        printf("%s\n", loadPassword().UTF8String);
      }
      return 0;
    }
    NSApplication* app = NSApplication.sharedApplication;
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    SSApp* delegate = [SSApp new];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
