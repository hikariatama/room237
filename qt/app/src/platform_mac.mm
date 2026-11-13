#include "platform.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>
#include <malloc/malloc.h>
#include <pthread.h>
namespace Platform {
// Offscreen QA has no native views: its window ids are not NSView pointers.
static bool nativeViews() { return QGuiApplication::platformName() == QLatin1String("cocoa"); }
QString fileManagerName() { return QStringLiteral("Finder"); }
QByteArray fileManagerIcon() { return "brand-finder"; }
void revealInFileManager(const QString &path) {
  @autoreleasepool {
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:path.toNSString()] ]];
  }
}
bool canProtectContent() { return true; }
void setContentProtected(QWidget *window, bool enabled) {
  if (!nativeViews()) return;
  auto *view = reinterpret_cast<NSView *>(window->winId());
  view.window.sharingType = enabled ? NSWindowSharingNone : NSWindowSharingReadOnly;
}
bool reduceMotion() { return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion]; }
void configureWindow(QWidget *window) {
  if (!nativeViews()) return;
  auto *view = reinterpret_cast<NSView *>(window->winId());
  NSWindow *native = view.window;
  native.titlebarAppearsTransparent = YES;
  native.titleVisibility = NSWindowTitleHidden;
  native.styleMask |= NSWindowStyleMaskFullSizeContentView;
  native.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  // The whole window has rounded corners.
  native.colorSpace = NSColorSpace.sRGBColorSpace;
  native.opaque = NO;
  native.backgroundColor = NSColor.clearColor;
  view.wantsLayer = YES;
  view.layer.cornerRadius = 24;
  view.layer.masksToBounds = YES;
  layoutWindowControls(window);
  // AppKit puts the buttons back on key changes and title bar layouts, not only on resize.
  NSButton *close = [native standardWindowButton:NSWindowCloseButton];
  close.postsFrameChangedNotifications = YES;
  auto reapply = ^(NSNotification *) {
    static bool laying = false;
    if (laying) return;
    laying = true;
    dispatch_async(dispatch_get_main_queue(), ^{
      layoutWindowControls(window);
      laying = false;
    });
  };
  NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
  [center addObserverForName:NSViewFrameDidChangeNotification object:close queue:nil usingBlock:reapply];
  for (NSNotificationName name in @[ NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification, NSWindowDidBecomeMainNotification, NSWindowDidResignMainNotification, NSWindowDidExitFullScreenNotification ])
    [center addObserverForName:name object:native queue:nil usingBlock:reapply];
}
int titleBarInset() { return 36; }
void activateApplication() { [NSApp activateIgnoringOtherApps:YES]; }
void roundSurface(QWindow *surface, int radius) {
  if (!nativeViews()) return;
  auto *view = reinterpret_cast<NSView *>(surface->winId());
  view.wantsLayer = YES;
  view.layer.cornerRadius = radius;
  view.layer.masksToBounds = YES;
}
// Traffic lights at (20, 20). AppKit resets the frames on resize, so this runs after each one.
void layoutWindowControls(QWidget *window) {
  if (!nativeViews()) return;
  auto *view = reinterpret_cast<NSView *>(window->winId());
  NSWindow *native = view.window;
  const bool fullScreen = native.styleMask & NSWindowStyleMaskFullScreen;
  view.layer.cornerRadius = fullScreen ? 0 : 24;
  [native invalidateShadow];
  if (fullScreen) return;
  NSButton *close = [native standardWindowButton:NSWindowCloseButton];
  NSButton *mini = [native standardWindowButton:NSWindowMiniaturizeButton];
  NSButton *zoom = [native standardWindowButton:NSWindowZoomButton];
  NSView *container = close.superview.superview;
  if (!container) return;
  constexpr CGFloat left = 20, top = 20;
  const CGFloat height = close.frame.size.height + 2 * top;
  NSRect frame = container.frame;
  frame.size.height = height;
  frame.origin.y = native.frame.size.height - height;
  container.frame = frame;
  const CGFloat spacing = mini.frame.origin.x - close.frame.origin.x;
  CGFloat x = left;
  for (NSButton *button in @[ close, mini, zoom ]) {
    [button setFrameOrigin:NSMakePoint(x, (close.superview.frame.size.height - close.frame.size.height) / 2)];
    x += spacing;
  }
}
void setDockVisibleProgress(double) {}
void lowerThreadPriority() { pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); }
void releaseFreeMemory() { malloc_zone_pressure_relief(nullptr, 0); }
static QImage decodeFromSource(CGImageSourceRef source, int maxPixelSize, QSize *fullSize);
QImage decodeImageNative(const QString &path, int maxPixelSize, QSize *fullSize) {
  @autoreleasepool {
    NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
    return decodeFromSource(CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr), maxPixelSize, fullSize);
  }
}
QImage decodeImageNativeData(const QByteArray &bytes, int maxPixelSize, QSize *fullSize) {
  @autoreleasepool {
    NSData *data = [NSData dataWithBytesNoCopy:const_cast<char *>(bytes.constData()) length:NSUInteger(bytes.size()) freeWhenDone:NO];
    return decodeFromSource(CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr), maxPixelSize, fullSize);
  }
}
static QImage decodeFromSource(CGImageSourceRef source, int maxPixelSize, QSize *fullSize) {
  @autoreleasepool {
    if (!source) return {};
    if (fullSize) {
      NSDictionary *properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
      int w = [properties[(id)kCGImagePropertyPixelWidth] intValue], h = [properties[(id)kCGImagePropertyPixelHeight] intValue];
      if ([properties[(id)kCGImagePropertyOrientation] intValue] >= 5) std::swap(w, h);
      *fullSize = QSize(w, h);
    }
    NSDictionary *options = @{(id)kCGImageSourceCreateThumbnailWithTransform : @YES, (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES, (id)kCGImageSourceThumbnailMaxPixelSize : @(maxPixelSize > 0 ? maxPixelSize : 32768)};
    CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)options);
    CFRelease(source);
    if (!image) return {};
    const int w = int(CGImageGetWidth(image)), h = int(CGImageGetHeight(image));
    const CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
    const bool opaque = alpha == kCGImageAlphaNone || alpha == kCGImageAlphaNoneSkipFirst || alpha == kCGImageAlphaNoneSkipLast;
    QImage out(w, h, opaque ? QImage::Format_RGB32 : QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) {
      CGImageRelease(image);
      return {};
    }
    out.fill(Qt::transparent);
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(out.bits(), w, h, 8, out.bytesPerLine(), rgb, (opaque ? kCGImageAlphaNoneSkipFirst : kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little);
    CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
    CGContextRelease(context);
    CGColorSpaceRelease(rgb);
    CGImageRelease(image);
    return out;
  }
}
}
