// Screen Share for macOS – native (AppKit), without Electron or Chromium.
//
// Home: share the screen (sharing password, which display, quality, system sound) or watch a room
// (code or link). Sharing: the room code and link, the viewers with their connection and latency.
// Watching: the picture fills the window, the bar over it fades away (F / double click: full
// screen, S: statistics, M: mute). About: version, updates, the open-source parts.
//
// The look is glass: the window is see-through to the desktop (blurred), coloured light behind,
// the panels frosted glass over it (Liquid Glass on macOS 26, the system's blur before). Screens
// cross-fade, buttons light up, the spinner turns on the GPU.
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
static NSString* const kSite = @"https://mozizas.hu";
static NSString* const kReleases = @"https://github.com/Shreklyx/screeen/releases/tag/desktop-latest";

static NSColor* rgb(uint32_t hex, CGFloat a = 1) {
  return [NSColor colorWithSRGBRed:CGFloat((hex >> 16) & 0xFF) / 255.0 green:CGFloat((hex >> 8) & 0xFF) / 255.0 blue:CGFloat(hex & 0xFF) / 255.0 alpha:a];
}

static const uint32_t kAccent = 0xFF4D6A, kAccent2 = 0x8B5CF6, kOk = 0x4ADE80, kWarn = 0xFBBF24, kErr = 0xFF6B7A;
static NSColor* textColor() { return rgb(0xEFEFF6); }
static NSColor* mutedColor() { return rgb(0xA0A0B6); }
static NSColor* faintColor() { return rgb(0x7A7A92); }

struct Quality {
  const char* name;
  const char* hint;
  int w, h, fps;
};
static const Quality kQualities[] = {
    {"Automatikus", "A legszebb, amit a gép és a hálózat bír: akár 4K, 60 FPS.", 3840, 2160, 60},
    {"1440p · 60", "2560×1440, 60 FPS.", 2560, 1440, 60},
    {"1080p · 60", "1920×1080, 60 FPS.", 1920, 1080, 60},
    {"1080p · 120", "1920×1080, 120 FPS – játékhoz, gyors mozgáshoz.", 1920, 1080, 120},
    {"720p · 60", "1280×720, 60 FPS – gyenge internetre.", 1280, 720, 60},
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

/* ------------------------------------------------ small helpers */

static NSImage* symbol(NSString* name, CGFloat size, NSFontWeight weight = NSFontWeightSemibold) {
  if (!name) return nil;
  NSImage* img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
  if (!img) return nil;
  return [img imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:size weight:weight]];
}

static NSTextField* label(NSString* text, CGFloat size, NSFontWeight weight, NSColor* color, bool wrap = false) {
  NSTextField* l = wrap ? [NSTextField wrappingLabelWithString:text] : [NSTextField labelWithString:text];
  l.font = [NSFont systemFontOfSize:size weight:weight];
  l.textColor = color;
  l.translatesAutoresizingMaskIntoConstraints = NO;
  if (!wrap) l.lineBreakMode = NSLineBreakByTruncatingTail;
  [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  return l;
}

static void pin(NSView* v, NSView* to, CGFloat inset = 0) {
  v.translatesAutoresizingMaskIntoConstraints = NO;
  [NSLayoutConstraint activateConstraints:@[
    [v.leadingAnchor constraintEqualToAnchor:to.leadingAnchor constant:inset], [v.trailingAnchor constraintEqualToAnchor:to.trailingAnchor constant:-inset],
    [v.topAnchor constraintEqualToAnchor:to.topAnchor constant:inset], [v.bottomAnchor constraintEqualToAnchor:to.bottomAnchor constant:-inset]
  ]];
}

static NSStackView* vstack(NSArray<NSView*>* views, CGFloat spacing) {
  NSStackView* s = [NSStackView stackViewWithViews:views];
  s.orientation = NSUserInterfaceLayoutOrientationVertical;
  s.alignment = NSLayoutAttributeLeading;
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

/** The child as wide as the (vertical) stack. */
static void stretch(NSView* child, NSStackView* stack) {
  [child.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-(stack.edgeInsets.left + stack.edgeInsets.right)].active = YES;
}

static NSView* spacer() {
  NSView* v = [NSView new];
  v.translatesAutoresizingMaskIntoConstraints = NO;
  [v setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
  [v setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
  return v;
}

static void animateAlpha(NSView* v, CGFloat to, double seconds = 0.22) {
  [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
    ctx.duration = seconds;
    ctx.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
    v.animator.alphaValue = to;
  }];
}

/* ------------------------------------------------ glass */

/** A frosted-glass panel: Liquid Glass (macOS 26) or the system blur of what is behind it, a faint
 *  white sheen, an edge that catches the light, a soft shadow. Content goes into `content`. */
@interface SSGlass : NSView
@property(nonatomic, readonly) NSView* content;
- (instancetype)initWithRadius:(CGFloat)radius strength:(CGFloat)strength;
@end

@implementation SSGlass
- (instancetype)initWithRadius:(CGFloat)radius strength:(CGFloat)strength {
  if ((self = [super initWithFrame:NSZeroRect])) {
    self.translatesAutoresizingMaskIntoConstraints = NO;
    self.wantsLayer = YES;
    NSShadow* sh = [NSShadow new];
    sh.shadowColor = rgb(0x000000, 0.45);
    sh.shadowBlurRadius = 22;
    sh.shadowOffset = NSMakeSize(0, -6);
    self.shadow = sh;
    NSView* blur = nil;
    Class glass = NSClassFromString(@"NSGlassEffectView");
    if (glass) {
      blur = [glass new];
      @try {
        [blur setValue:@(radius) forKey:@"cornerRadius"];
      } @catch (NSException*) {
      }
    } else {
      NSVisualEffectView* v = [NSVisualEffectView new];
      v.material = NSVisualEffectMaterialHUDWindow;
      v.blendingMode = NSVisualEffectBlendingModeWithinWindow;
      v.state = NSVisualEffectStateActive;
      v.wantsLayer = YES;
      v.layer.cornerRadius = radius;
      v.layer.masksToBounds = YES;
      blur = v;
    }
    NSView* sheen = [NSView new];
    CAGradientLayer* g = [CAGradientLayer layer];
    g.colors = @[ (id)rgb(0xFFFFFF, 0.11 * strength).CGColor, (id)rgb(0xFFFFFF, 0.04 * strength).CGColor, (id)rgb(0xFFFFFF, 0.02 * strength).CGColor ];
    g.locations = @[ @0, @0.4, @1 ];
    g.startPoint = CGPointMake(0.5, 1); // (layer coordinates: y up)
    g.endPoint = CGPointMake(0.5, 0);
    g.cornerRadius = radius;
    g.borderWidth = 1;
    g.borderColor = rgb(0xFFFFFF, 0.17 * strength).CGColor;
    sheen.layer = g; // layer-hosting: the layer first, then wantsLayer
    sheen.wantsLayer = YES;
    _content = [NSView new];
    [self addSubview:blur];
    pin(blur, self);
    [self addSubview:sheen];
    pin(sheen, self);
    [self addSubview:_content];
    pin(_content, self);
  }
  return self;
}
@end

/** The coloured light behind the glass: soft radial glows in the window's corners. */
@interface SSLights : NSView
@end

@implementation SSLights {
  NSArray<CAGradientLayer*>* _glows;
}
- (instancetype)initWithFrame:(NSRect)r {
  if ((self = [super initWithFrame:r])) {
    self.wantsLayer = YES;
    NSMutableArray* list = [NSMutableArray array];
    const uint32_t colors[4] = {kAccent, kAccent2, 0x3B82F6, 0x22D3EE};
    const CGFloat alphas[4] = {0.50, 0.46, 0.38, 0.20};
    for (int i = 0; i < 4; i++) {
      CAGradientLayer* g = [CAGradientLayer layer];
      g.type = kCAGradientLayerRadial;
      g.colors = @[ (id)rgb(colors[i], alphas[i]).CGColor, (id)rgb(colors[i], 0).CGColor ];
      g.startPoint = CGPointMake(0.5, 0.5);
      g.endPoint = CGPointMake(1, 1);
      [self.layer addSublayer:g];
      [list addObject:g];
    }
    _glows = list;
  }
  return self;
}
- (BOOL)isFlipped {
  return YES;
}
- (void)layout {
  [super layout];
  CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
  // centre (as a share of the size) and radius
  const CGFloat spots[4][3] = {{0.12, 0.84, 560}, {0.90, 0.70, 600}, {0.52, -0.05, 640}, {0.0, 0.14, 380}};
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  for (NSUInteger i = 0; i < _glows.count; i++) {
    CGFloat r = spots[i][2];
    _glows[i].frame = CGRectMake(W * spots[i][0] - r, H * spots[i][1] - r, 2 * r, 2 * r);
  }
  [CATransaction commit];
}
- (NSView*)hitTest:(NSPoint)p {
  return nil; // light only, not in the way of clicks
}
@end

/* ------------------------------------------------ buttons, chips */

typedef NS_ENUM(NSInteger, SSStyle) { SSPrimary, SSSecondary, SSDanger, SSGhost, SSChip, SSSuccess };

@interface SSButton : NSView
@property(nonatomic) SSStyle style;
@property(nonatomic) BOOL selected;
@property(nonatomic) BOOL enabled;
@property(nonatomic, copy) void (^onClick)(void);
- (instancetype)initWithTitle:(NSString*)title symbol:(NSString*)sym style:(SSStyle)style;
- (void)setTitle:(NSString*)title symbol:(NSString*)sym;
@end

@implementation SSButton {
  CAGradientLayer* _fill;
  CAGradientLayer* _gloss;
  CALayer* _hover;
  NSTextField* _label;
  NSImageView* _icon;
  NSStackView* _row;
  BOOL _inside, _down;
  NSTrackingArea* _tracking;
}

- (instancetype)initWithTitle:(NSString*)title symbol:(NSString*)sym style:(SSStyle)style {
  if ((self = [super initWithFrame:NSZeroRect])) {
    self.translatesAutoresizingMaskIntoConstraints = NO;
    self.wantsLayer = YES;
    _enabled = YES;
    _fill = [CAGradientLayer layer];
    _fill.startPoint = CGPointMake(0, 0.5);
    _fill.endPoint = CGPointMake(1, 0.5);
    _fill.borderWidth = 1;
    _gloss = [CAGradientLayer layer];
    _gloss.colors = @[ (id)rgb(0xFFFFFF, 0.26).CGColor, (id)rgb(0xFFFFFF, 0.03).CGColor, (id)rgb(0xFFFFFF, 0).CGColor ];
    _gloss.locations = @[ @0, @0.5, @1 ];
    _gloss.startPoint = CGPointMake(0.5, 1);
    _gloss.endPoint = CGPointMake(0.5, 0);
    _hover = [CALayer layer];
    _hover.backgroundColor = rgb(0xFFFFFF, 1).CGColor;
    _hover.opacity = 0;
    [self.layer addSublayer:_fill];
    [self.layer addSublayer:_gloss];
    [self.layer addSublayer:_hover];
    _icon = [NSImageView new];
    _icon.translatesAutoresizingMaskIntoConstraints = NO;
    _label = label(@"", 14, NSFontWeightSemibold, textColor());
    _label.alignment = NSTextAlignmentCenter;
    _row = hstack(@[ _icon, _label ], 8);
    [self addSubview:_row];
    [NSLayoutConstraint activateConstraints:@[
      [_row.centerXAnchor constraintEqualToAnchor:self.centerXAnchor], [_row.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
      [_row.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.leadingAnchor constant:12],
      [_row.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-12],
    ]];
    _style = style;
    [self setTitle:title symbol:sym];
  }
  return self;
}

- (void)setTitle:(NSString*)title symbol:(NSString*)sym {
  _label.stringValue = title ?: @"";
  _label.hidden = title.length == 0;
  NSImage* img = symbol(sym, 14);
  _icon.image = img;
  _icon.hidden = img == nil;
  [self restyle];
  [self invalidateIntrinsicContentSize];
}

- (NSSize)intrinsicContentSize {
  NSSize s = _row.fittingSize;
  return NSMakeSize(s.width + 32, _style == SSChip ? 36 : 44);
}

- (void)setStyle:(SSStyle)style {
  _style = style;
  [self restyle];
}
- (void)setSelected:(BOOL)selected {
  _selected = selected;
  [self restyle];
}
- (void)setEnabled:(BOOL)enabled {
  _enabled = enabled;
  self.alphaValue = enabled ? 1 : 0.55;
  [self restyle];
}

- (void)restyle {
  NSColor* text = textColor();
  NSArray* colors;
  NSColor* border;
  BOOL gloss = NO;
  switch (_style) {
    case SSPrimary:
      colors = @[ (id)rgb(kAccent).CGColor, (id)rgb(kAccent2).CGColor ];
      border = rgb(0xFFFFFF, 0.35);
      gloss = YES;
      text = NSColor.whiteColor;
      break;
    case SSSuccess:
      colors = @[ (id)rgb(0x174A30, 0.85).CGColor, (id)rgb(0x123222, 0.85).CGColor ];
      border = rgb(kOk, 0.6);
      text = rgb(kOk);
      break;
    case SSDanger:
      colors = @[ (id)rgb(0x5A1426, 0.6).CGColor, (id)rgb(0x3A0F1C, 0.6).CGColor ];
      border = rgb(kErr, 0.5);
      text = rgb(0xFFC9D0);
      break;
    case SSSecondary:
      colors = @[ (id)rgb(0xFFFFFF, 0.10).CGColor, (id)rgb(0xFFFFFF, 0.06).CGColor ];
      border = rgb(0xFFFFFF, 0.22);
      break;
    case SSGhost:
      colors = @[ (id)NSColor.clearColor.CGColor, (id)NSColor.clearColor.CGColor ];
      border = NSColor.clearColor;
      text = mutedColor();
      break;
    case SSChip:
      colors = _selected ? @[ (id)rgb(kAccent, 0.32).CGColor, (id)rgb(kAccent2, 0.26).CGColor ] : @[ (id)rgb(0xFFFFFF, 0.05).CGColor, (id)rgb(0xFFFFFF, 0.05).CGColor ];
      border = _selected ? rgb(0xFF8DA0, 0.9) : rgb(0xFFFFFF, 0.14);
      text = _selected ? textColor() : mutedColor();
      break;
  }
  [CATransaction begin];
  [CATransaction setAnimationDuration:0.18];
  _fill.colors = colors;
  _fill.borderColor = border.CGColor;
  _gloss.hidden = !gloss;
  [CATransaction commit];
  _label.textColor = text;
  _label.font = [NSFont systemFontOfSize:_style == SSChip ? 13 : 14 weight:(_style == SSChip && !_selected) ? NSFontWeightMedium : NSFontWeightSemibold];
  _icon.contentTintColor = text;
}

- (void)layout {
  [super layout];
  CGFloat radius = std::min<CGFloat>(12, self.bounds.size.height / 2);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  for (CALayer* l in @[ _fill, _gloss, _hover ]) {
    l.frame = self.bounds;
    l.cornerRadius = _style == SSChip ? 10 : radius;
  }
  [CATransaction commit];
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:self userInfo:nil];
  [self addTrackingArea:_tracking];
}

- (void)resetCursorRects {
  if (_enabled) [self addCursorRect:self.bounds cursor:NSCursor.pointingHandCursor];
}

- (void)light {
  [CATransaction begin];
  [CATransaction setAnimationDuration:_down ? 0.06 : 0.16];
  _hover.opacity = !_enabled ? 0 : _down ? 0.16 : _inside ? 0.08 : 0;
  [CATransaction commit];
}

- (void)mouseEntered:(NSEvent*)e {
  _inside = YES;
  [self light];
}
- (void)mouseExited:(NSEvent*)e {
  _inside = NO;
  [self light];
}
- (void)mouseDown:(NSEvent*)e {
  if (!_enabled) return;
  _down = YES;
  [self light];
}
- (void)mouseUp:(NSEvent*)e {
  if (!_down) return;
  _down = NO;
  [self light];
  NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  if (NSPointInRect(p, self.bounds) && _enabled && _onClick) _onClick();
}
- (BOOL)acceptsFirstMouse:(NSEvent*)e {
  return YES;
}
@end

/* ------------------------------------------------ a text box in a dark well */

@interface SSField : NSView <NSTextFieldDelegate>
@property(nonatomic, readonly) NSTextField* field;
@property(nonatomic, copy) void (^onEnter)(void);
@property(nonatomic, copy) void (^onEdit)(void);
- (instancetype)initSecure:(BOOL)secure symbol:(NSString*)sym placeholder:(NSString*)placeholder;
@end

@implementation SSField {
  CALayer* _well;
  CALayer* _ring;
}
- (instancetype)initSecure:(BOOL)secure symbol:(NSString*)sym placeholder:(NSString*)placeholder {
  if ((self = [super initWithFrame:NSZeroRect])) {
    self.translatesAutoresizingMaskIntoConstraints = NO;
    self.wantsLayer = YES;
    _well = [CALayer layer];
    _well.backgroundColor = rgb(0x0E0E18, 0.78).CGColor;
    _well.borderWidth = 1;
    _well.borderColor = rgb(0xFFFFFF, 0.14).CGColor;
    _well.cornerRadius = 10;
    _ring = [CALayer layer];
    _ring.borderWidth = 3;
    _ring.borderColor = rgb(kAccent, 0.0).CGColor;
    _ring.cornerRadius = 13;
    [self.layer addSublayer:_ring];
    [self.layer addSublayer:_well];
    NSImageView* icon = [NSImageView imageViewWithImage:symbol(sym, 13, NSFontWeightMedium) ?: [NSImage new]];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.contentTintColor = faintColor();
    _field = secure ? [NSSecureTextField new] : [NSTextField new];
    _field.translatesAutoresizingMaskIntoConstraints = NO;
    _field.bezeled = NO;
    _field.bordered = NO;
    _field.drawsBackground = NO;
    _field.focusRingType = NSFocusRingTypeNone;
    _field.font = [NSFont systemFontOfSize:14];
    _field.textColor = textColor();
    _field.delegate = self;
    if (placeholder)
      _field.placeholderAttributedString = [[NSAttributedString alloc] initWithString:placeholder
                                                                           attributes:@{NSForegroundColorAttributeName : faintColor(), NSFontAttributeName : _field.font}];
    [self addSubview:icon];
    [self addSubview:_field];
    [NSLayoutConstraint activateConstraints:@[
      [self.heightAnchor constraintEqualToConstant:42], [icon.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14],
      [icon.centerYAnchor constraintEqualToAnchor:self.centerYAnchor], [icon.widthAnchor constraintEqualToConstant:18],
      [_field.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:10], [_field.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-12],
      [_field.centerYAnchor constraintEqualToAnchor:self.centerYAnchor]
    ]];
  }
  return self;
}
- (void)layout {
  [super layout];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _well.frame = self.bounds;
  _ring.frame = NSInsetRect(self.bounds, -3, -3);
  [CATransaction commit];
}
- (void)focus:(BOOL)on {
  [CATransaction begin];
  [CATransaction setAnimationDuration:0.18];
  _ring.borderColor = rgb(kAccent, on ? 0.28 : 0).CGColor;
  _well.borderColor = on ? rgb(0xFF8DA0, 0.9).CGColor : rgb(0xFFFFFF, 0.14).CGColor;
  [CATransaction commit];
}
- (void)controlTextDidBeginEditing:(NSNotification*)n {
  [self focus:YES];
}
- (void)controlTextDidEndEditing:(NSNotification*)n {
  [self focus:NO];
}
- (void)controlTextDidChange:(NSNotification*)n {
  if (_onEdit) _onEdit();
}
- (BOOL)control:(NSControl*)c textView:(NSTextView*)tv doCommandBySelector:(SEL)sel {
  if (sel == @selector(insertNewline:) && _onEnter) {
    _onEnter();
    return YES;
  }
  return NO;
}
- (void)mouseDown:(NSEvent*)e {
  [self.window makeFirstResponder:_field];
}
@end

/* ------------------------------------------------ a spinner (turns on the GPU), a status dot */

@interface SSSpinner : NSView
- (instancetype)initWithSize:(CGFloat)size color:(NSColor*)color;
@end

@implementation SSSpinner {
  CAShapeLayer* _arc;
  CAShapeLayer* _track;
  CGFloat _size;
}
- (instancetype)initWithSize:(CGFloat)size color:(NSColor*)color {
  if ((self = [super initWithFrame:NSMakeRect(0, 0, size, size)])) {
    self.translatesAutoresizingMaskIntoConstraints = NO;
    self.wantsLayer = YES;
    _size = size;
    [NSLayoutConstraint activateConstraints:@[ [self.widthAnchor constraintEqualToConstant:size], [self.heightAnchor constraintEqualToConstant:size] ]];
    CGFloat w = std::max<CGFloat>(2, size / 12);
    CGPathRef full = CGPathCreateWithEllipseInRect(CGRectInset(CGRectMake(0, 0, size, size), w, w), nullptr);
    _track = [CAShapeLayer layer];
    _track.path = full;
    _track.fillColor = nil;
    _track.strokeColor = [color colorWithAlphaComponent:0.15].CGColor;
    _track.lineWidth = w;
    _arc = [CAShapeLayer layer];
    _arc.path = full;
    _arc.fillColor = nil;
    _arc.strokeColor = color.CGColor;
    _arc.lineWidth = w;
    _arc.lineCap = kCALineCapRound;
    _arc.strokeEnd = 0.32;
    CGPathRelease(full);
    _arc.frame = CGRectMake(0, 0, size, size);
    _track.frame = _arc.frame;
    [self.layer addSublayer:_track];
    [self.layer addSublayer:_arc];
    CABasicAnimation* spin = [CABasicAnimation animationWithKeyPath:@"transform.rotation.z"];
    spin.fromValue = @0;
    spin.toValue = @(-2 * M_PI);
    spin.duration = 0.9;
    spin.repeatCount = HUGE_VALF;
    [_arc addAnimation:spin forKey:@"spin"];
    CABasicAnimation* breathe = [CABasicAnimation animationWithKeyPath:@"strokeEnd"];
    breathe.fromValue = @0.18;
    breathe.toValue = @0.62;
    breathe.duration = 0.85;
    breathe.autoreverses = YES;
    breathe.repeatCount = HUGE_VALF;
    breathe.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [_arc addAnimation:breathe forKey:@"breathe"];
  }
  return self;
}
- (void)layout {
  [super layout];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _arc.position = CGPointMake(self.bounds.size.width / 2, self.bounds.size.height / 2);
  _arc.bounds = CGRectMake(0, 0, _size, _size);
  _track.frame = self.bounds;
  [CATransaction commit];
}
@end

static NSView* makeDot(NSColor* color, CGFloat size = 9) {
  NSView* d = [NSView new];
  d.translatesAutoresizingMaskIntoConstraints = NO;
  d.wantsLayer = YES;
  d.layer.backgroundColor = color.CGColor;
  d.layer.cornerRadius = size / 2;
  d.layer.shadowColor = color.CGColor;
  d.layer.shadowOpacity = 0.8;
  d.layer.shadowRadius = 5;
  d.layer.shadowOffset = CGSizeZero;
  [NSLayoutConstraint activateConstraints:@[ [d.widthAnchor constraintEqualToConstant:size], [d.heightAnchor constraintEqualToConstant:size] ]];
  return d;
}

static void setDotColor(NSView* d, NSColor* color) {
  d.layer.backgroundColor = color.CGColor;
  d.layer.shadowColor = color.CGColor;
}

/** A switch with its text. */
static NSStackView* switchRow(NSSwitch* __strong* out, NSString* text, BOOL on, id target, SEL action) {
  NSSwitch* s = [NSSwitch new];
  s.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  s.target = target;
  s.action = action;
  s.controlSize = NSControlSizeSmall;
  *out = s;
  return hstack(@[ s, label(text, 13.5, NSFontWeightRegular, textColor()) ], 10);
}

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

@interface SSViewerRow : SSGlass
- (void)update:(const Json&)v;
@end

@implementation SSViewerRow {
  NSView* _dot;
  NSTextField *_name, *_sub;
  NSStackView* _pills;
}
- (instancetype)init {
  if ((self = [super initWithRadius:16 strength:0.85])) {
    _dot = makeDot(rgb(kOk));
    _name = label(@"", 14, NSFontWeightSemibold, textColor());
    _sub = label(@"", 12, NSFontWeightRegular, mutedColor());
    _pills = hstack(@[], 6);
    NSStackView* text = vstack(@[ _name, _sub ], 2);
    NSStackView* row = hstack(@[ _dot, text, spacer(), _pills ], 12);
    row.edgeInsets = NSEdgeInsetsMake(0, 18, 0, 14);
    [self.content addSubview:row];
    pin(row, self.content);
    [self.heightAnchor constraintEqualToConstant:58].active = YES;
  }
  return self;
}

static NSString* pathName(const std::string& p) {
  static const std::map<std::string, std::string> names = {
      {"lan", "helyi hálózat"}, {"internet", "interneten át"}, {"relay", "a szerveren át"}, {"turn", "TURN relén át"}, {"tcp", "HTTPS-en át"}};
  auto it = names.find(p);
  return S(it == names.end() ? p : it->second);
}

static NSView* pill(NSString* text, NSColor* color) {
  NSView* v = [NSView new];
  v.translatesAutoresizingMaskIntoConstraints = NO;
  v.wantsLayer = YES;
  v.layer.backgroundColor = rgb(0xFFFFFF, 0.07).CGColor;
  v.layer.cornerRadius = 13;
  NSTextField* l = label(text, 12, NSFontWeightSemibold, color);
  [v addSubview:l];
  [NSLayoutConstraint activateConstraints:@[
    [v.heightAnchor constraintEqualToConstant:26], [l.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:10],
    [l.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-10], [l.centerYAnchor constraintEqualToAnchor:v.centerYAnchor]
  ]];
  return v;
}

- (void)update:(const Json&)v {
  std::string state = v["state"].str("");
  bool connected = state == "connected";
  setDotColor(_dot, rgb(connected ? kOk : state == "unsupported" ? kErr : state == "retrying" ? kWarn : 0xA0A0B6));
  _name.stringValue = [NSString stringWithFormat:@"Néző #%d", int(v["n"].num())];
  _sub.stringValue = connected                 ? [@"közvetlen mód · " stringByAppendingString:pathName(v["path"].str(""))]
                     : state == "unsupported" ? [@"nem tud nézni: " stringByAppendingString:S(v["whyText"].str(""))]
                     : state == "retrying"    ? [@"kapcsolat újra… " stringByAppendingString:S(v["whyText"].str(""))]
                                              : @"kapcsolódik…";
  for (NSView* p in _pills.arrangedSubviews) [p removeFromSuperview];
  if (!connected) return;
  if (v["fps"].isNumber()) [_pills addArrangedSubview:pill([NSString stringWithFormat:@"%d FPS", int(v["fps"].num())], textColor())];
  if (v["kbps"].isNumber()) [_pills addArrangedSubview:pill([NSString stringWithFormat:@"%.1f Mbit/s", v["kbps"].num() / 1000], textColor())];
  if (v["rtt"].isNumber()) [_pills addArrangedSubview:pill([NSString stringWithFormat:@"RTT %d ms", int(std::lround(v["rtt"].num()))], textColor())];
  if (v["lat"].isNumber()) {
    double lat = v["lat"].num();
    [_pills addArrangedSubview:pill([NSString stringWithFormat:@"késés ~%d ms", int(std::lround(lat))], rgb(lat < 40 ? kOk : lat < 90 ? kWarn : kErr))];
  }
}
@end

/* ------------------------------------------------ the app */

@interface SSApp : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation SSApp {
  NSWindow* _window;
  NSView *_stage, *_current;
  NSView *_home, *_host, *_viewer, *_about;
  // home
  SSField *_password, *_code, *_server;
  NSSwitch *_remember, *_audio;
  NSMutableArray<SSButton*>*_monitorChips, *_qualityChips;
  NSTextField *_qualityHint, *_shareError, *_joinError;
  SSButton* _updateButton;
  int _monitor, _quality;
  std::vector<MacDisplay> _displays;
  // sharing
  NSTextField *_roomCode, *_link, *_status, *_info, *_badgeText, *_viewerCount;
  NSView *_statusDot, *_badge, *_badgeDot, *_emptyViewers;
  SSSpinner* _statusSpinner;
  SSButton *_pauseButton, *_copyButton;
  NSStackView* _viewerList;
  std::map<std::string, SSViewerRow*> _rows;
  // watching
  SSVideoView* _video;
  NSView *_bar, *_overlay, *_viewDot;
  NSTextField *_viewTitle, *_viewInfo, *_overlayText;
  NSSlider* _volume;
  SSButton* _muteButton;
  double _lastMove;
  // about
  SSButton *_checkButton, *_aboutUpdate;
  NSTextField* _updateNote;
  SSSpinner* _checkSpinner;
  // the dialog
  NSView* _modal;
  NSTextField *_modalTitle, *_modalText;
  SSButton *_modalOk, *_modalCancel;
  void (^_modalAction)(void);

  std::unique_ptr<Engine> _engine;
  std::unique_ptr<MacHost> _hostPipe;
  std::unique_ptr<MacPlayer> _player;
  std::unique_ptr<MacAudioOut> _audioOut;
  int _session;
  bool _live, _picture, _stats, _muted, _checking;
  std::string _roomLink, _statusKind;
  Json _viewStats;
  NSTimer* _timer;
  NSString *_updateFile, *_updateVersion;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
  [NSApp setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
  [self makeMenu];
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1100, 780)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                                                  NSWindowStyleMaskFullSizeContentView
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"Screen Share";
  _window.titlebarAppearsTransparent = YES;
  _window.titleVisibility = NSWindowTitleHidden;
  _window.minSize = NSMakeSize(980, 720);
  _window.delegate = self;
  _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
  // the window: the desktop behind it, blurred and darkened; coloured light; the screens; the dialog
  NSVisualEffectView* root = [NSVisualEffectView new];
  root.material = NSVisualEffectMaterialUnderWindowBackground;
  root.blendingMode = NSVisualEffectBlendingModeBehindWindow;
  root.state = NSVisualEffectStateActive;
  _window.contentView = root;
  NSView* shade = [NSView new];
  shade.wantsLayer = YES;
  shade.layer.backgroundColor = rgb(0x06060C, 0.55).CGColor;
  [root addSubview:shade];
  pin(shade, root);
  SSLights* lights = [SSLights new];
  [root addSubview:lights];
  pin(lights, root);
  _stage = [SSFlipped new];
  _stage.wantsLayer = YES;
  [root addSubview:_stage];
  pin(_stage, root);
  _live = true;
  _monitor = 0;
  _quality = int(std::min<NSInteger>([NSUserDefaults.standardUserDefaults integerForKey:@"quality"], NSInteger(std::size(kQualities)) - 1));
  [self makeHome];
  [self makeHost];
  [self makeViewer];
  [self makeAbout];
  [self makeModal:root];
  [self show:_home];
  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  _timer = [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(barTick) userInfo:nil repeats:YES];
  [self checkUpdatesLater];
  __weak SSApp* weak = self;
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                        handler:^NSEvent*(NSEvent* e) {
                                          SSApp* s = weak;
                                          return s ? [s key:e] : e;
                                        }];
}

- (NSEvent*)key:(NSEvent*)e {
  if (e.modifierFlags & NSEventModifierFlagCommand) return e;
  NSString* k = e.charactersIgnoringModifiers.lowercaseString;
  if (!_modal.hidden) {
    if (e.keyCode == 53) [self closeModal:NO]; // Esc
    else if (e.keyCode == 36) [self closeModal:YES]; // Return
    return nil;
  }
  if (_current == _viewer) {
    if ([k isEqualToString:@"f"]) [_window toggleFullScreen:nil];
    else if ([k isEqualToString:@"s"]) {
      _stats = !_stats;
      [self tick];
    } else if ([k isEqualToString:@"m"]) [self setMuted:!_muted];
    else return e;
    [self moved];
    return nil;
  }
  if (_current == _about && e.keyCode == 53) {
    [self show:_home];
    return nil;
  }
  return e;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a {
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
  [view addItemWithTitle:@"Teljes képernyő" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask =
      NSEventModifierFlagCommand | NSEventModifierFlagControl;
  viewItem.submenu = view;
  NSApp.mainMenu = bar;
}

/* ---- pieces */

- (NSView*)logo:(CGFloat)size {
  NSView* v = [NSView new];
  v.translatesAutoresizingMaskIntoConstraints = NO;
  NSView* bg = [NSView new]; // the gradient: a layer-hosting view of its own (no subviews in it)
  CAGradientLayer* g = [CAGradientLayer layer];
  g.colors = @[ (id)rgb(kAccent).CGColor, (id)rgb(kAccent2).CGColor ];
  g.startPoint = CGPointMake(0, 1);
  g.endPoint = CGPointMake(1, 0);
  g.cornerRadius = size * 0.3;
  g.borderWidth = 1;
  g.borderColor = rgb(0xFFFFFF, 0.4).CGColor;
  g.shadowColor = rgb(kAccent).CGColor;
  g.shadowOpacity = 0.5;
  g.shadowRadius = size * 0.35;
  g.shadowOffset = CGSizeZero;
  bg.layer = g;
  bg.wantsLayer = YES;
  [v addSubview:bg];
  pin(bg, v);
  NSImageView* icon = [NSImageView imageViewWithImage:symbol(@"display", size * 0.42, NSFontWeightSemibold) ?: [NSImage new]];
  icon.translatesAutoresizingMaskIntoConstraints = NO;
  icon.contentTintColor = NSColor.whiteColor;
  [v addSubview:icon];
  [NSLayoutConstraint activateConstraints:@[
    [v.widthAnchor constraintEqualToConstant:size], [v.heightAnchor constraintEqualToConstant:size], [icon.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
    [icon.centerYAnchor constraintEqualToAnchor:v.centerYAnchor]
  ]];
  return v;
}

- (NSView*)badgeIcon:(NSString*)sym color:(uint32_t)color {
  NSView* v = [NSView new];
  v.translatesAutoresizingMaskIntoConstraints = NO;
  v.wantsLayer = YES;
  v.layer.backgroundColor = rgb(color, 0.17).CGColor;
  v.layer.cornerRadius = 11;
  NSImageView* icon = [NSImageView imageViewWithImage:symbol(sym, 16) ?: [NSImage new]];
  icon.translatesAutoresizingMaskIntoConstraints = NO;
  icon.contentTintColor = rgb(color == kAccent2 ? 0xB49CFF : color);
  [v addSubview:icon];
  [NSLayoutConstraint activateConstraints:@[
    [v.widthAnchor constraintEqualToConstant:38], [v.heightAnchor constraintEqualToConstant:38], [icon.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
    [icon.centerYAnchor constraintEqualToAnchor:v.centerYAnchor]
  ]];
  return v;
}

- (NSStackView*)cardTitle:(NSString*)title sub:(NSString*)sub symbol:(NSString*)sym color:(uint32_t)color {
  return hstack(@[ [self badgeIcon:sym color:color], vstack(@[ label(title, 17, NSFontWeightSemibold, textColor()), label(sub, 12.5, NSFontWeightRegular, mutedColor()) ], 2) ], 14);
}

- (NSTextField*)caption:(NSString*)text {
  return label(text, 12.5, NSFontWeightMedium, mutedColor());
}

/** The screen's content: centred, at most `maxWidth` wide, below the title bar. */
- (NSView*)screenWith:(NSView*)content maxWidth:(CGFloat)maxWidth top:(CGFloat)top {
  NSView* screen = [SSFlipped new];
  [screen addSubview:content];
  content.translatesAutoresizingMaskIntoConstraints = NO;
  NSLayoutConstraint* full = [content.widthAnchor constraintEqualToAnchor:screen.widthAnchor constant:-72];
  full.priority = NSLayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    full, [content.widthAnchor constraintLessThanOrEqualToConstant:maxWidth], [content.centerXAnchor constraintEqualToAnchor:screen.centerXAnchor],
    [content.topAnchor constraintEqualToAnchor:screen.topAnchor constant:top]
  ]];
  return screen;
}

- (SSButton*)chip:(NSString*)title action:(void (^)(void))action {
  SSButton* b = [[SSButton alloc] initWithTitle:title symbol:nil style:SSChip];
  b.onClick = action;
  return b;
}

/* ---- home */

- (void)makeHome {
  __weak SSApp* weak = self;
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  // header
  NSTextField* title = label(@"Screen Share", 26, NSFontWeightBold, textColor());
  NSTextField* sub = label(@"4K képernyőmegosztás hanggal · saját, titkosított átvitel", 13, NSFontWeightRegular, mutedColor());
  SSButton* about = [[SSButton alloc] initWithTitle:@"Névjegy" symbol:@"info.circle" style:SSGhost];
  about.onClick = ^{ [weak showAbout]; };
  NSStackView* header = hstack(@[ [self logo:46], vstack(@[ title, sub ], 2), spacer(), about ], 14);
  // sharing
  _password = [[SSField alloc] initSecure:YES symbol:@"lock" placeholder:@"jelszó"];
  _password.field.stringValue = loadPassword();
  _password.onEnter = ^{ [weak share]; };
  NSStackView* rememberRow = switchRow(&_remember, @"Jelszó megjegyzése", _password.field.stringValue.length > 0, nil, nil);
  _displays = listDisplays();
  _monitorChips = [NSMutableArray array];
  NSStackView* monitors = vstack(@[], 8);
  NSStackView* monitorRow = nil;
  for (size_t i = 0; i < _displays.size(); i++) {
    if (i % 2 == 0) {
      monitorRow = hstack(@[], 8);
      [monitors addArrangedSubview:monitorRow];
    }
    int index = int(i);
    SSButton* c = [self chip:S(_displays[i].name) action:^{ [weak pickMonitor:index]; }];
    [_monitorChips addObject:c];
    [monitorRow addArrangedSubview:c];
  }
  if (_displays.empty()) [monitors addArrangedSubview:label(@"Nem találok képernyőt.", 13, NSFontWeightRegular, rgb(kErr))];
  _qualityChips = [NSMutableArray array];
  NSStackView* qualities = vstack(@[], 8);
  NSStackView* qrow = nil;
  for (int i = 0; i < int(std::size(kQualities)); i++) {
    if (i % 3 == 0) {
      qrow = hstack(@[], 8);
      qrow.distribution = NSStackViewDistributionFillEqually;
      [qualities addArrangedSubview:qrow];
      stretch(qrow, qualities);
    }
    SSButton* c = [self chip:@(kQualities[i].name) action:^{ [weak pickQuality:i]; }];
    [_qualityChips addObject:c];
    [qrow addArrangedSubview:c];
  }
  _qualityHint = label(@"", 12, NSFontWeightRegular, faintColor());
  NSStackView* audioRow = switchRow(&_audio, @"A gép hangja is (macOS 13-tól)", !([ud objectForKey:@"audio"] && ![ud boolForKey:@"audio"]), nil, nil);
  SSButton* shareButton = [[SSButton alloc] initWithTitle:@"Megosztás indítása" symbol:@"play.fill" style:SSPrimary];
  shareButton.onClick = ^{ [weak share]; };
  [shareButton.heightAnchor constraintEqualToConstant:48].active = YES;
  _shareError = label(@"", 13, NSFontWeightMedium, rgb(kErr), true);
  _shareError.hidden = YES;
  NSStackView* share = vstack(@[
    [self cardTitle:@"Képernyő megosztása" sub:@"A képernyőd és a gép hangja – a lehető legkisebb késéssel." symbol:@"display" color:kAccent],
    [self caption:@"Megosztási jelszó"], _password, rememberRow, [self caption:@"Képernyő"], monitors, [self caption:@"Minőség"], qualities, _qualityHint, audioRow,
    shareButton, _shareError
  ],
                              12);
  share.edgeInsets = NSEdgeInsetsMake(24, 24, 24, 24);
  [share setCustomSpacing:20 afterView:share.arrangedSubviews[0]];
  [share setCustomSpacing:16 afterView:rememberRow];
  [share setCustomSpacing:16 afterView:monitors];
  [share setCustomSpacing:16 afterView:audioRow];
  for (NSView* v in @[ _password, qualities, shareButton, _shareError ]) stretch(v, share);
  SSGlass* shareCard = [[SSGlass alloc] initWithRadius:20 strength:1];
  [shareCard.content addSubview:share];
  pin(share, shareCard.content);
  // watching
  _code = [[SSField alloc] initSecure:NO symbol:@"link" placeholder:@"pl. abc123 vagy https://mozizas.hu/#abc123"];
  _code.onEnter = ^{ [weak join]; };
  _code.onEdit = ^{ [weak clearJoinError]; };
  SSButton* joinButton = [[SSButton alloc] initWithTitle:@"Csatlakozás" symbol:@"eye" style:SSPrimary];
  joinButton.onClick = ^{ [weak join]; };
  [joinButton.heightAnchor constraintEqualToConstant:48].active = YES;
  _joinError = label(@"", 13, NSFontWeightMedium, rgb(kErr), true);
  _joinError.hidden = YES;
  NSBox* line = [NSBox new];
  line.boxType = NSBoxSeparator;
  line.translatesAutoresizingMaskIntoConstraints = NO;
  _server = [[SSField alloc] initSecure:NO symbol:@"bolt.fill" placeholder:@"https://mozizas.hu"];
  _server.field.stringValue = [ud stringForKey:@"server"] ?: @"https://mozizas.hu";
  NSStackView* join = vstack(@[
    [self cardTitle:@"Megosztás nézése" sub:@"Írd be a szobakódot, vagy illeszd be a kapott linket." symbol:@"eye" color:kAccent2],
    [self caption:@"Szobakód vagy link"], _code, joinButton, _joinError, line, [self caption:@"Szerver"], _server,
    label(@"Csak akkor írd át, ha saját szervered van.", 12, NSFontWeightRegular, faintColor()), spacer()
  ],
                             12);
  join.edgeInsets = NSEdgeInsetsMake(24, 24, 24, 24);
  [join setCustomSpacing:20 afterView:join.arrangedSubviews[0]];
  [join setCustomSpacing:16 afterView:_code];
  [join setCustomSpacing:26 afterView:_joinError];
  [join setCustomSpacing:22 afterView:line];
  for (NSView* v in @[ _code, joinButton, _joinError, line, _server ]) stretch(v, join);
  SSGlass* joinCard = [[SSGlass alloc] initWithRadius:20 strength:1];
  [joinCard.content addSubview:join];
  pin(join, joinCard.content);
  NSStackView* cards = hstack(@[ shareCard, joinCard ], 20);
  cards.alignment = NSLayoutAttributeTop;
  cards.distribution = NSStackViewDistributionFillEqually;
  [joinCard.heightAnchor constraintEqualToAnchor:shareCard.heightAnchor].active = YES;
  // footer
  _updateButton = [[SSButton alloc] initWithTitle:@"" symbol:@"arrow.down.circle" style:SSPrimary];
  _updateButton.onClick = ^{ [weak installUpdate]; };
  _updateButton.hidden = YES;
  NSStackView* footer =
      hstack(@[ label([NSString stringWithFormat:@"Screen Share %s · natív alkalmazás", SS_VERSION], 12, NSFontWeightRegular, faintColor()), spacer(), _updateButton ], 12);
  NSStackView* all = vstack(@[ header, cards, footer ], 22);
  for (NSView* v in @[ header, cards, footer ]) stretch(v, all);
  _home = [self screenWith:all maxWidth:1060 top:48];
  [self pickMonitor:0];
  [self pickQuality:_quality];
}

- (void)pickMonitor:(int)i {
  _monitor = i;
  for (NSUInteger k = 0; k < _monitorChips.count; k++) _monitorChips[k].selected = int(k) == i;
}

- (void)pickQuality:(int)i {
  _quality = i;
  for (NSUInteger k = 0; k < _qualityChips.count; k++) _qualityChips[k].selected = int(k) == i;
  _qualityHint.stringValue = @(kQualities[i].hint);
}

- (void)setError:(NSTextField*)l text:(NSString*)text {
  l.stringValue = text ?: @"";
  if (text.length) {
    l.alphaValue = 0;
    l.hidden = NO;
    animateAlpha(l, 1);
  } else {
    l.hidden = YES;
  }
}

- (void)clearJoinError {
  if (!_joinError.hidden) [self setError:_joinError text:@""];
}

/* ---- sharing */

- (void)makeHost {
  __weak SSApp* weak = self;
  NSTextField* title = label(@"Képernyőmegosztás", 24, NSFontWeightBold, textColor());
  _badgeDot = makeDot(rgb(kAccent), 7);
  _badgeText = label(@"ÉLŐ", 11.5, NSFontWeightBold, rgb(0xFFB3BF));
  NSStackView* badgeRow = hstack(@[ _badgeDot, _badgeText ], 7);
  badgeRow.edgeInsets = NSEdgeInsetsMake(0, 10, 0, 12);
  _badge = badgeRow;
  _badge.wantsLayer = YES;
  _badge.layer.backgroundColor = rgb(kAccent, 0.18).CGColor;
  _badge.layer.cornerRadius = 12;
  [_badge.heightAnchor constraintEqualToConstant:24].active = YES;
  _pauseButton = [[SSButton alloc] initWithTitle:@"Szünet" symbol:@"pause.fill" style:SSSecondary];
  _pauseButton.onClick = ^{ [weak togglePause]; };
  SSButton* stop = [[SSButton alloc] initWithTitle:@"Megosztás vége" symbol:@"stop.fill" style:SSDanger];
  stop.onClick = ^{ [weak askStop]; };
  NSStackView* header = hstack(@[ title, _badge, spacer(), _pauseButton, stop ], 12);
  // the room
  _roomCode = label(@"······", 46, NSFontWeightBold, rgb(0xFF8DA0));
  _link = label(@"A link a kapcsolódás után jelenik meg…", 14, NSFontWeightRegular, faintColor());
  NSView* well = [NSView new];
  well.translatesAutoresizingMaskIntoConstraints = NO;
  well.wantsLayer = YES;
  well.layer.backgroundColor = rgb(0x0E0E18, 0.78).CGColor;
  well.layer.cornerRadius = 12;
  well.layer.borderWidth = 1;
  well.layer.borderColor = rgb(0xFFFFFF, 0.14).CGColor;
  NSImageView* linkIcon = [NSImageView imageViewWithImage:symbol(@"link", 13, NSFontWeightMedium) ?: [NSImage new]];
  linkIcon.contentTintColor = faintColor();
  NSStackView* linkInner = hstack(@[ linkIcon, _link ], 10);
  [well addSubview:linkInner];
  [NSLayoutConstraint activateConstraints:@[
    [well.heightAnchor constraintEqualToConstant:46], [linkInner.leadingAnchor constraintEqualToAnchor:well.leadingAnchor constant:16],
    [linkInner.trailingAnchor constraintLessThanOrEqualToAnchor:well.trailingAnchor constant:-12], [linkInner.centerYAnchor constraintEqualToAnchor:well.centerYAnchor]
  ]];
  _copyButton = [[SSButton alloc] initWithTitle:@"Link másolása" symbol:@"doc.on.doc" style:SSPrimary];
  _copyButton.onClick = ^{ [weak copyLink]; };
  [_copyButton.widthAnchor constraintEqualToConstant:190].active = YES;
  [_copyButton.heightAnchor constraintEqualToConstant:46].active = YES;
  NSStackView* linkRow = hstack(@[ well, _copyButton ], 12);
  _statusSpinner = [[SSSpinner alloc] initWithSize:14 color:mutedColor()];
  _statusDot = makeDot(rgb(kOk));
  _status = label(@"", 13.5, NSFontWeightMedium, textColor());
  NSStackView* statusRow = hstack(@[ _statusSpinner, _statusDot, _status ], 10);
  _info = label(@"", 12.5, NSFontWeightRegular, mutedColor());
  NSStackView* room = vstack(@[ label(@"SZOBAKÓD", 11.5, NSFontWeightBold, faintColor()), _roomCode, linkRow, statusRow, _info ], 10);
  room.edgeInsets = NSEdgeInsetsMake(26, 26, 26, 26);
  [room setCustomSpacing:2 afterView:room.arrangedSubviews[0]];
  [room setCustomSpacing:16 afterView:linkRow];
  [room setCustomSpacing:4 afterView:statusRow];
  stretch(linkRow, room);
  SSGlass* roomCard = [[SSGlass alloc] initWithRadius:20 strength:1];
  [roomCard.content addSubview:room];
  pin(room, roomCard.content);
  // viewers
  NSImageView* users = [NSImageView imageViewWithImage:symbol(@"person.2", 15) ?: [NSImage new]];
  users.contentTintColor = mutedColor();
  _viewerCount = label(@"0", 12, NSFontWeightBold, mutedColor());
  NSView* count = [NSView new];
  count.translatesAutoresizingMaskIntoConstraints = NO;
  count.wantsLayer = YES;
  count.layer.backgroundColor = rgb(0xFFFFFF, 0.08).CGColor;
  count.layer.cornerRadius = 11;
  [count addSubview:_viewerCount];
  [NSLayoutConstraint activateConstraints:@[
    [count.heightAnchor constraintEqualToConstant:22], [count.widthAnchor constraintGreaterThanOrEqualToConstant:28],
    [_viewerCount.centerXAnchor constraintEqualToAnchor:count.centerXAnchor], [_viewerCount.centerYAnchor constraintEqualToAnchor:count.centerYAnchor],
    [_viewerCount.leadingAnchor constraintGreaterThanOrEqualToAnchor:count.leadingAnchor constant:8]
  ]];
  NSStackView* viewersHead = hstack(@[ users, label(@"Nézők", 16, NSFontWeightSemibold, textColor()), count ], 8);
  SSGlass* empty = [[SSGlass alloc] initWithRadius:18 strength:0.6];
  NSImageView* emptyIcon = [NSImageView imageViewWithImage:symbol(@"person.2", 26, NSFontWeightRegular) ?: [NSImage new]];
  emptyIcon.contentTintColor = faintColor();
  NSStackView* emptyStack = vstack(@[ emptyIcon, label(@"Még nincs néző. Küldd el a linket vagy a kódot!", 14, NSFontWeightMedium, mutedColor()) ], 10);
  emptyStack.alignment = NSLayoutAttributeCenterX;
  [empty.content addSubview:emptyStack];
  [NSLayoutConstraint activateConstraints:@[
    [empty.heightAnchor constraintEqualToConstant:116], [emptyStack.centerXAnchor constraintEqualToAnchor:empty.content.centerXAnchor],
    [emptyStack.centerYAnchor constraintEqualToAnchor:empty.content.centerYAnchor]
  ]];
  _emptyViewers = empty;
  _viewerList = vstack(@[ _emptyViewers ], 8);
  stretch(_emptyViewers, _viewerList);
  NSStackView* all = vstack(@[ header, roomCard, viewersHead, _viewerList ], 20);
  [all setCustomSpacing:26 afterView:roomCard];
  [all setCustomSpacing:12 afterView:viewersHead];
  for (NSView* v in @[ header, roomCard, _viewerList ]) stretch(v, all);
  _host = [self screenWith:all maxWidth:1000 top:48];
}

/* ---- watching */

- (void)makeViewer {
  __weak SSApp* weak = self;
  _viewer = [SSFlipped new];
  _video = [[SSVideoView alloc] initWithFrame:NSZeroRect];
  _video.onDoubleClick = ^{ [weak fullScreen]; };
  _video.onMouseMove = ^{ [weak moved]; };
  [_viewer addSubview:_video];
  pin(_video, _viewer);
  // the overlay: a spinner and what is happening
  SSSpinner* spin = [[SSSpinner alloc] initWithSize:40 color:rgb(kAccent)];
  _overlayText = label(@"", 15, NSFontWeightMedium, textColor(), true);
  _overlayText.alignment = NSTextAlignmentCenter;
  NSStackView* overlay = vstack(@[ spin, _overlayText ], 16);
  overlay.alignment = NSLayoutAttributeCenterX;
  _overlay = overlay;
  [_viewer addSubview:_overlay];
  [NSLayoutConstraint activateConstraints:@[
    [_overlay.centerXAnchor constraintEqualToAnchor:_viewer.centerXAnchor], [_overlay.centerYAnchor constraintEqualToAnchor:_viewer.centerYAnchor],
    [_overlayText.widthAnchor constraintLessThanOrEqualToConstant:520]
  ]];
  // the bar over the picture: glass, it fades away while the mouse rests
  NSVisualEffectView* bar = [NSVisualEffectView new];
  bar.material = NSVisualEffectMaterialHUDWindow;
  bar.blendingMode = NSVisualEffectBlendingModeWithinWindow;
  bar.state = NSVisualEffectStateActive;
  bar.translatesAutoresizingMaskIntoConstraints = NO;
  _bar = bar;
  _viewDot = makeDot(rgb(kWarn));
  _viewTitle = label(@"", 14.5, NSFontWeightSemibold, textColor());
  _viewInfo = label(@"", 12.5, NSFontWeightRegular, mutedColor());
  _muteButton = [[SSButton alloc] initWithTitle:@"" symbol:@"speaker.wave.2.fill" style:SSGhost];
  _muteButton.onClick = ^{ [weak toggleMute]; };
  _volume = [NSSlider sliderWithValue:100 minValue:0 maxValue:100 target:self action:@selector(volumeChanged)];
  _volume.trackFillColor = rgb(kAccent);
  _volume.controlSize = NSControlSizeSmall;
  [_volume.widthAnchor constraintEqualToConstant:120].active = YES;
  SSButton* full = [[SSButton alloc] initWithTitle:@"" symbol:@"arrow.up.left.and.arrow.down.right" style:SSGhost];
  full.onClick = ^{ [weak fullScreen]; };
  SSButton* leave = [[SSButton alloc] initWithTitle:@"Kilépés" symbol:@"xmark" style:SSDanger];
  leave.onClick = ^{ [weak stopToHome]; };
  [leave.heightAnchor constraintEqualToConstant:36].active = YES;
  NSStackView* row = hstack(@[ _viewDot, _viewTitle, _viewInfo, spacer(), _muteButton, _volume, full, leave ], 12);
  [row setCustomSpacing:14 afterView:_viewTitle];
  [bar addSubview:row];
  [NSLayoutConstraint activateConstraints:@[
    [row.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:84], [row.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-14],
    [row.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor]
  ]];
  [_viewer addSubview:_bar];
  [NSLayoutConstraint activateConstraints:@[
    [_bar.topAnchor constraintEqualToAnchor:_viewer.topAnchor], [_bar.leadingAnchor constraintEqualToAnchor:_viewer.leadingAnchor],
    [_bar.trailingAnchor constraintEqualToAnchor:_viewer.trailingAnchor], [_bar.heightAnchor constraintEqualToConstant:56]
  ]];
}

- (void)moved {
  _lastMove = nowMs();
  if (_bar.alphaValue < 1) animateAlpha(_bar, 1, 0.18);
}

- (void)barTick {
  // the bar fades away while the picture plays and the mouse rests
  if (_current != _viewer || !_picture) return;
  if (nowMs() - _lastMove > 2600 && _bar.alphaValue > 0 && _modal.hidden) {
    animateAlpha(_bar, 0, 0.5);
    if (_window.styleMask & NSWindowStyleMaskFullScreen) [NSCursor setHiddenUntilMouseMoves:YES];
  }
}

/* ---- about */

- (NSView*)feature:(NSString*)sym title:(NSString*)title text:(NSString*)text {
  return hstack(@[ [self badgeIcon:sym color:kAccent2], vstack(@[ label(title, 14, NSFontWeightSemibold, textColor()), label(text, 12.5, NSFontWeightRegular, mutedColor()) ], 2) ],
                12);
}

- (void)makeAbout {
  __weak SSApp* weak = self;
  SSButton* back = [[SSButton alloc] initWithTitle:@"Vissza" symbol:@"chevron.left" style:SSGhost];
  back.onClick = ^{
    SSApp* me = weak;
    if (me) [me show:me->_home];
  };
  NSStackView* head = hstack(@[
    [self logo:64], vstack(@[
      label(@"Screen Share", 30, NSFontWeightBold, textColor()),
      label([NSString stringWithFormat:@"Verzió %s · natív macOS-alkalmazás (Apple Silicon és Intel)", SS_VERSION], 13, NSFontWeightRegular, mutedColor())
    ],
                           4)
  ],
                             18);
  NSTextField* about = label(@"Képernyőmegosztás hanggal, a lehető legkisebb késéssel. A képet a gép hardveres kódolója készíti, és saját, végig titkosított "
                             @"átvitellel megy a nézőkhöz – két alkalmazás között közvetlenül, puffer nélkül. Böngésző, Electron és Chromium nélkül.",
                             14, NSFontWeightRegular, textColor(), true);
  NSStackView* f1 = hstack(@[ [self feature:@"bolt.fill" title:@"Kis késés" text:@"Közvetlen UDP-kapcsolat, pufferek nélkül"],
                              [self feature:@"display" title:@"Akár 4K 60 FPS" text:@"VideoToolbox hardveres kódolás"] ],
                           20);
  NSStackView* f2 = hstack(@[ [self feature:@"waveform" title:@"A gép hangja" text:@"macOS 13-tól, semmit nem kell telepíteni"],
                              [self feature:@"lock.fill" title:@"Végig titkosítva" text:@"X25519 + AES-GCM – a szerver sem látja a képet"] ],
                           20);
  for (NSStackView* f in @[ f1, f2 ]) {
    f.distribution = NSStackViewDistributionFillEqually;
    f.alignment = NSLayoutAttributeTop;
  }
  NSBox* line = [NSBox new];
  line.boxType = NSBoxSeparator;
  line.translatesAutoresizingMaskIntoConstraints = NO;
  _checkButton = [[SSButton alloc] initWithTitle:@"Frissítések keresése" symbol:@"arrow.clockwise" style:SSSecondary];
  _checkButton.onClick = ^{ [weak checkUpdatesNow]; };
  _aboutUpdate = [[SSButton alloc] initWithTitle:@"" symbol:@"arrow.down.circle" style:SSPrimary];
  _aboutUpdate.onClick = ^{ [weak installUpdate]; };
  _aboutUpdate.hidden = YES;
  _checkSpinner = [[SSSpinner alloc] initWithSize:16 color:mutedColor()];
  _checkSpinner.hidden = YES;
  _updateNote = label(@"", 13, NSFontWeightMedium, mutedColor());
  NSStackView* updates = hstack(@[ _checkButton, _aboutUpdate, _checkSpinner, _updateNote ], 14);
  SSButton* site = [[SSButton alloc] initWithTitle:@"mozizas.hu" symbol:@"link" style:SSGhost];
  site.onClick = ^{ [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:kSite]]; };
  SSButton* releases = [[SSButton alloc] initWithTitle:@"Letöltések (GitHub)" symbol:@"arrow.down.circle" style:SSGhost];
  releases.onClick = ^{ [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:kReleases]]; };
  NSStackView* links = hstack(@[ site, releases ], 4);
  NSTextField* credits = label(@"Nyílt forrású részek: BearSSL (MIT) – titkosítás · Opus (BSD) – hang.", 12, NSFontWeightRegular, faintColor(), true);
  NSStackView* card = vstack(@[ head, about, f1, f2, line, updates, links, credits ], 18);
  card.edgeInsets = NSEdgeInsetsMake(30, 30, 30, 30);
  [card setCustomSpacing:22 afterView:head];
  [card setCustomSpacing:24 afterView:about];
  [card setCustomSpacing:22 afterView:f2];
  for (NSView* v in @[ about, f1, f2, line, credits ]) stretch(v, card);
  SSGlass* glass = [[SSGlass alloc] initWithRadius:22 strength:1];
  [glass.content addSubview:card];
  pin(card, glass.content);
  NSStackView* all = vstack(@[ back, glass ], 16);
  stretch(glass, all);
  _about = [self screenWith:all maxWidth:760 top:40];
}

- (void)showAbout {
  [self show:_about];
}

/* ---- the dialog */

- (void)makeModal:(NSView*)root {
  __weak SSApp* weak = self;
  _modal = [SSFlipped new];
  _modal.wantsLayer = YES;
  _modal.layer.backgroundColor = rgb(0x05050A, 0.6).CGColor;
  _modal.hidden = YES;
  [root addSubview:_modal];
  pin(_modal, root);
  _modalTitle = label(@"", 18, NSFontWeightSemibold, textColor());
  _modalText = label(@"", 13.5, NSFontWeightRegular, mutedColor(), true);
  _modalCancel = [[SSButton alloc] initWithTitle:@"Mégse" symbol:nil style:SSSecondary];
  _modalCancel.onClick = ^{ [weak closeModal:NO]; };
  _modalOk = [[SSButton alloc] initWithTitle:@"" symbol:nil style:SSDanger];
  _modalOk.onClick = ^{ [weak closeModal:YES]; };
  NSStackView* buttons = hstack(@[ _modalCancel, _modalOk ], 12);
  buttons.distribution = NSStackViewDistributionFillEqually;
  NSStackView* box = vstack(@[ _modalTitle, _modalText, buttons ], 14);
  box.edgeInsets = NSEdgeInsetsMake(28, 28, 28, 28);
  [box setCustomSpacing:26 afterView:_modalText];
  stretch(_modalText, box);
  stretch(buttons, box);
  SSGlass* card = [[SSGlass alloc] initWithRadius:22 strength:1.4];
  [card.content addSubview:box];
  pin(box, card.content);
  [_modal addSubview:card];
  [NSLayoutConstraint activateConstraints:@[
    [card.widthAnchor constraintEqualToConstant:440], [card.centerXAnchor constraintEqualToAnchor:_modal.centerXAnchor],
    [card.centerYAnchor constraintEqualToAnchor:_modal.centerYAnchor]
  ]];
}

- (void)ask:(NSString*)title text:(NSString*)text ok:(NSString*)ok cancel:(NSString*)cancel danger:(BOOL)danger action:(void (^)(void))action {
  _modalTitle.stringValue = title;
  _modalText.stringValue = text;
  [_modalOk setTitle:ok symbol:nil];
  _modalOk.style = danger ? SSDanger : SSPrimary;
  _modalCancel.hidden = cancel == nil;
  _modalAction = action;
  _modal.alphaValue = 0;
  _modal.hidden = NO;
  [_window makeFirstResponder:nil];
  animateAlpha(_modal, 1, 0.2);
}

- (void)closeModal:(BOOL)confirmed {
  if (_modal.hidden) return;
  void (^action)(void) = _modalAction;
  _modalAction = nil;
  NSView* modal = _modal;
  [NSAnimationContext
      runAnimationGroup:^(NSAnimationContext* ctx) {
        ctx.duration = 0.16;
        modal.animator.alphaValue = 0;
      }
      completionHandler:^{
        modal.hidden = YES;
      }];
  if (confirmed && action) action();
}

/* ---- switching screens: a cross-fade */

- (void)show:(NSView*)v {
  if (!v || v == _current) return;
  CATransition* t = [CATransition animation];
  t.type = kCATransitionFade;
  t.duration = 0.26;
  t.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
  [_stage.layer addAnimation:t forKey:@"swap"];
  [_current removeFromSuperview];
  [_stage addSubview:v];
  pin(v, _stage);
  _current = v;
  if (v == _viewer) {
    [_window makeFirstResponder:_video];
    _bar.alphaValue = 1;
    _lastMove = nowMs();
  } else {
    [_window makeFirstResponder:nil];
  }
}

/* ---- sessions */

- (std::string)serverUrl {
  std::string s = U([_server.field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]);
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
  NSString* pw = _password.field.stringValue;
  if (!pw.length) {
    [self setError:_shareError text:@"Add meg a megosztási jelszót."];
    [_window makeFirstResponder:_password.field];
    return;
  }
  if (_displays.empty()) {
    [self setError:_shareError text:@"Nem találok képernyőt."];
    return;
  }
  NSUserDefaults* ud = NSUserDefaults.standardUserDefaults;
  [ud setInteger:_quality forKey:@"quality"];
  [ud setBool:_audio.state == NSControlStateValueOn forKey:@"audio"];
  [ud setObject:S([self serverUrl]) forKey:@"server"];
  savePassword(_remember.state == NSControlStateValueOn ? pw : @"");
  [self setError:_shareError text:@""];
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
  const Quality& q = kQualities[_quality];
  MacHost::Options o;
  if (_monitor >= 0 && _monitor < int(_displays.size())) o.display = _displays[size_t(_monitor)].id;
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
  _link.textColor = faintColor();
  _copyButton.enabled = NO;
  _info.stringValue = @"";
  [self setStatus:@"Kapcsolódás a szerverhez…" kind:""];
  [self updateBadge];
  [self updateViewers:Json::array()];
  [_pauseButton setTitle:@"Szünet" symbol:@"pause.fill"];
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
  [self watch:parseCode(_code.field.stringValue)];
}

- (void)watch:(const std::string&)code {
  if (code.size() < 4) {
    [self setError:_joinError text:@"Érvénytelen szobakód – írd be a 6 jelű kódot, vagy illeszd be a teljes linket."];
    [_window makeFirstResponder:_code.field];
    return;
  }
  [NSUserDefaults.standardUserDefaults setObject:S([self serverUrl]) forKey:@"server"];
  [self setError:_joinError text:@""];
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
  _overlayText.stringValue = @"Csatlakozás a szobához…";
  _overlay.alphaValue = 1;
  _overlay.hidden = NO;
  _viewTitle.stringValue = S("Szoba: " + code);
  _viewInfo.stringValue = @"";
  setDotColor(_viewDot, rgb(kWarn));
  [self show:_viewer];
  engine->join(code);
}

- (void)stopToHome {
  if (_window.styleMask & NSWindowStyleMaskFullScreen) [_window toggleFullScreen:nil];
  [self endSession];
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
  [_pauseButton setTitle:_live ? @"Szünet" : @"Folytatás" symbol:_live ? @"pause.fill" : @"play.fill"];
  [self updateBadge];
}

- (void)updateBadge {
  _badgeText.stringValue = _live ? @"ÉLŐ" : @"SZÜNETEL";
  _badgeText.textColor = _live ? rgb(0xFFB3BF) : mutedColor();
  setDotColor(_badgeDot, _live ? rgb(kAccent) : mutedColor());
  _badge.layer.backgroundColor = (_live ? rgb(kAccent, 0.18) : rgb(0xFFFFFF, 0.08)).CGColor;
}

- (void)copyLink {
  if (_roomLink.empty()) return;
  [NSPasteboard.generalPasteboard clearContents];
  [NSPasteboard.generalPasteboard setString:S(_roomLink) forType:NSPasteboardTypeString];
  [_copyButton setTitle:@"Kimásolva" symbol:@"checkmark"];
  _copyButton.style = SSSuccess;
  __weak SSButton* b = _copyButton;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    [b setTitle:@"Link másolása" symbol:@"doc.on.doc"];
    b.style = SSPrimary;
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
  [_muteButton setTitle:@"" symbol:m ? @"speaker.slash.fill" : @"speaker.wave.2.fill"];
}

/* ---- engine events (main thread) */

- (void)setStatus:(NSString*)text kind:(const std::string&)kind {
  _statusKind = kind;
  _status.stringValue = text ?: @"";
  bool connecting = kind.empty();
  _statusSpinner.hidden = !connecting;
  _statusDot.hidden = connecting;
  NSColor* c = kind == "ok" ? rgb(kOk) : kind == "warn" ? rgb(kWarn) : kind == "error" ? rgb(kErr) : textColor();
  _status.textColor = c;
  if (!connecting) setDotColor(_statusDot, c);
}

- (void)updateViewers:(const Json&)list {
  std::vector<Json> items = list.items();
  std::sort(items.begin(), items.end(), [](const Json& a, const Json& b) { return a["n"].num() < b["n"].num(); });
  std::map<std::string, bool> present;
  for (const Json& v : items) present[v["id"].str("")] = true;
  // the ones gone: out
  for (auto it = _rows.begin(); it != _rows.end();) {
    if (!present.count(it->first)) {
      SSViewerRow* row = it->second;
      [row removeFromSuperview];
      it = _rows.erase(it);
    } else {
      ++it;
    }
  }
  // the new ones fade in; all in their order
  NSUInteger index = 0;
  for (const Json& v : items) {
    std::string id = v["id"].str("");
    SSViewerRow* row = _rows.count(id) ? _rows[id] : nil;
    if (!row) {
      row = [SSViewerRow new];
      row.alphaValue = 0;
      _rows[id] = row;
      [_viewerList insertArrangedSubview:row atIndex:index];
      stretch(row, _viewerList);
      animateAlpha(row, 1, 0.3);
    }
    [row update:v];
    index++;
  }
  _emptyViewers.hidden = !items.empty();
  _viewerCount.stringValue = [NSString stringWithFormat:@"%d", int(items.size())];
  _viewerCount.textColor = items.empty() ? mutedColor() : rgb(0xFFB3BF);
  _viewerCount.superview.layer.backgroundColor = (items.empty() ? rgb(0xFFFFFF, 0.08) : rgb(kAccent, 0.2)).CGColor;
}

- (void)onEvent:(const Json&)e {
  const std::string& type = e["type"].str();
  if (type == "status") {
    [self setStatus:S(e["text"].str()) kind:e["kind"].str()];
  } else if (type == "hosted") {
    NSMutableAttributedString* code = [[NSMutableAttributedString alloc] initWithString:S(e["code"].str()) attributes:@{
      NSFontAttributeName : [NSFont systemFontOfSize:46 weight:NSFontWeightBold],
      NSForegroundColorAttributeName : rgb(0xFF8DA0),
      NSKernAttributeName : @6
    }];
    _roomCode.attributedStringValue = code;
    _roomLink = e["link"].str();
    _link.stringValue = S(_roomLink);
    _link.textColor = textColor();
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
    [self setError:_joinError text:@"A szobát egy másik ablak vette át."];
  } else if (type == "view-state" && _current == _viewer) {
    bool picture = e["picture"].b();
    if (picture != _picture) {
      _picture = picture;
      setDotColor(_viewDot, rgb(picture ? kOk : kWarn));
      if (picture) {
        animateAlpha(_overlay, 0, 0.25);
        _lastMove = nowMs();
      } else {
        _overlay.hidden = NO;
        animateAlpha(_overlay, 1, 0.25);
        animateAlpha(_bar, 1, 0.2);
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
    _info.stringValue = S(s);
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
      } else {
        s += " · S: részletek";
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
  [self show:_about];
  [self checkUpdates:YES];
}

- (void)checkUpdates:(BOOL)asked {
  if (_checking) return;
  _checking = true;
  if (asked) {
    _checkSpinner.hidden = NO;
    _updateNote.stringValue = @"";
    _checkButton.enabled = NO;
  }
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
  _checkSpinner.hidden = YES;
  _checkButton.enabled = YES;
  if (version) {
    _updateVersion = version;
    _updateFile = file;
    NSString* title = [NSString stringWithFormat:@"Frissítés: %@", version];
    [_updateButton setTitle:title symbol:@"arrow.down.circle"];
    [_aboutUpdate setTitle:title symbol:@"arrow.down.circle"];
    _updateButton.alphaValue = 0;
    _updateButton.hidden = NO;
    animateAlpha(_updateButton, 1, 0.3);
    _aboutUpdate.hidden = NO;
    _checkButton.hidden = YES;
    _updateNote.stringValue = @"Új verzió érhető el.";
    _updateNote.textColor = rgb(kOk);
  } else if (asked) {
    _updateNote.stringValue = failed ? @"Nem érem el a frissítéseket – próbáld később." : @"Ez a legújabb verzió.";
    _updateNote.textColor = mutedColor();
  }
}

- (void)installUpdate {
  NSString* file = _updateFile;
  if (!file) return;
  for (SSButton* b in @[ _updateButton, _aboutUpdate ]) {
    [b setTitle:@"Letöltés…" symbol:@"arrow.down.circle"];
    b.enabled = NO;
  }
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
                                        for (SSButton* b in @[ self->_updateButton, self->_aboutUpdate ]) {
                                          [b setTitle:@"Frissítés sikertelen – újra" symbol:@"arrow.clockwise"];
                                          b.enabled = YES;
                                        }
                                        return;
                                      }
                                      // after we quit: the new app in place of this one, then start it
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
  [self ask:@"Kilépsz?" text:@"A megosztás befejeződik: a nézőknél megáll a kép, és a szoba bezárul." ok:@"Kilépés" cancel:@"Mégse" danger:YES action:^{
    [self endSession];
    [win close];
  }];
  return NO;
}

- (void)windowDidExitFullScreen:(NSNotification*)n {
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
