#include "qa_native_panel.h"
#include "qa_fence.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <unistd.h>
#include <QThread>
#include <QUuid>
#include <QWidget>
#include <sys/stat.h>


static QString panelFileIdentity(const QString &path) {
  struct stat value;
  if (::stat(QFile::encodeName(path).constData(), &value) || !S_ISDIR(value.st_mode)) return {};
  return QStringLiteral("%1:%2").arg(qulonglong(value.st_dev)).arg(qulonglong(value.st_ino));
}
// The URLs getter is read before the button action. This delegate is the last gate on the
// actual URL AppKit tries to accept, so stale getter data cannot authorize another folder.
@interface Room237QaDirectoryDelegate : NSObject <NSOpenSavePanelDelegate> {
@public
  id<NSOpenSavePanelDelegate> previous;
  QString approvedPath, approvedIdentity;
  bool armed, validated;
}
@end
@implementation Room237QaDirectoryDelegate
- (BOOL)respondsToSelector:(SEL)selector {
  return [super respondsToSelector:selector] || [previous respondsToSelector:selector];
}
- (id)forwardingTargetForSelector:(SEL)selector {
  return [previous respondsToSelector:selector] ? previous : [super forwardingTargetForSelector:selector];
}
- (BOOL)panel:(id)sender validateURL:(NSURL *)url error:(NSError **)error {
  const QString path = url.isFileURL ? QString::fromNSString(url.path) : QString();
  const bool allowed = armed && QaFence::allows(path) && QFileInfo(path).isDir() &&
      QFileInfo(path).canonicalFilePath() == approvedPath && panelFileIdentity(path) == approvedIdentity;
  if (!allowed) {
    if (error) *error = [NSError errorWithDomain:@"Room237QA" code:1 userInfo:@{NSLocalizedDescriptionKey:@"QA refused changed or unverified folder"}];
    return NO;
  }
  if ([previous respondsToSelector:@selector(panel:validateURL:error:)] && ![previous panel:sender validateURL:url error:error]) return NO;
  validated = true;
  return YES;
}
- (void)dealloc { [(id)previous release]; [super dealloc]; }
@end

namespace QaNativePanel {
static NSOpenPanel *bound = nil;
static Room237QaDirectoryDelegate *validation = nil;
static NSInteger number = 0;
static QString lease, selected, identity;
static QElapsedTimer ticketClock;
void retire() {
  if (bound && validation && bound.delegate == validation) bound.delegate = validation->previous;
  [validation release]; validation = nil;
  [bound release];
  bound = nil;
  number = 0;
  lease.clear(); selected.clear(); identity.clear(); ticketClock.invalidate();
}
static bool present() {
  return bound && [NSApp.windows containsObject:bound] && bound.visible && bound.windowNumber == number;
}

static QString selection(QString *error) {
  if (!present() || !bound.canChooseDirectories || bound.canChooseFiles || bound.allowsMultipleSelection) {
    *error = QStringLiteral("unknown/stale directory panel configuration"); return {};
  }
  // AppKit's selected URLs are authoritative; never interpret text, AX labels or directoryURL as a selection.
  NSArray<NSURL *> *urls = bound.URLs;
  if (urls.count != 1 || !urls.firstObject.isFileURL) {
    *error = QStringLiteral("unknown/ambiguous native selection URLs"); return {};
  }
  const QString path = QString::fromNSString(urls.firstObject.path);
  if (!QaFence::allows(path) || !QFileInfo(path).isDir()) {
    *error = QStringLiteral("QA fence refused selected directory"); return {};
  }
  return QFileInfo(path).canonicalFilePath();
}
static bool rectVisible(NSRect rect, QString *why);
static bool actionVisible(NSView *button, QString *why) {
  if (!button || button.hidden) { *why = QStringLiteral("button hidden"); return false; }
  return rectVisible([bound convertRectToScreen:[button convertRect:button.bounds toView:nil]], why);
}
// The panel's own window at the centre of its content, for remote panels whose controls live in a view service.
static bool panelVisible(QString *why) {
  const NSRect content = [bound contentRectForFrameRect:bound.frame];
  return rectVisible(NSMakeRect(NSMidX(content) - 8, NSMidY(content) - 8, 16, 16), why);
}
static bool rectVisible(NSRect rect, QString *why) {
  // Same refusal policy as qa_guard: never activate; a foreign banner over the action means refusal.
  if (NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier != getpid()) { *why = QStringLiteral("not frontmost"); return false; }
  NSScreen *main = NSScreen.screens.firstObject, *screen = nil;
  for (NSScreen *candidate in NSScreen.screens)
    if (NSContainsRect(candidate.visibleFrame, rect)) screen = candidate;
  if (!main || !screen) { *why = QStringLiteral("button outside every visible frame"); return false; }
  CGPoint point = CGPointMake(NSMidX(rect), NSMaxY(main.frame) - NSMidY(rect));
  NSArray *windows = (NSArray *)CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
  bool ok = false;
  for (NSDictionary *window in windows) {
    CGRect frame;
    if (!CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)window[(id)kCGWindowBounds], &frame) || !CGRectContainsPoint(frame, point)) continue;
    // The Dock keeps a full-screen layer-20 window above every normal window; events pass through it (same rule as qa_guard).
    const int owner = [window[(id)kCGWindowOwnerPID] intValue];
    if ([window[(id)kCGWindowLayer] intValue] == 20 && [[NSRunningApplication runningApplicationWithProcessIdentifier:owner].bundleIdentifier isEqualToString:@"com.apple.dock"]) {
      bool fullScreen = false;
      for (NSScreen *candidate in NSScreen.screens)
        fullScreen |= CGRectEqualToRect(frame, CGRectMake(NSMinX(candidate.frame), NSMaxY(main.frame) - NSMaxY(candidate.frame), NSWidth(candidate.frame), NSHeight(candidate.frame)));
      if (fullScreen) continue;
    }
    // Reason only: own panel / other own window / a system component by bundle id / other. No foreign titles or geometry.
    ok = owner == getpid() && [window[(id)kCGWindowNumber] integerValue] == number;
    if (!ok) {
      NSString *bundle = [NSRunningApplication runningApplicationWithProcessIdentifier:owner].bundleIdentifier;
      *why = owner == getpid() ? QStringLiteral("own window %1 above panel %2").arg([window[(id)kCGWindowNumber] integerValue]).arg(number)
             : [bundle hasPrefix:@"com.apple."] ? QStringLiteral("system window above button: %1 layer %2").arg(QString::fromNSString(bundle)).arg([window[(id)kCGWindowLayer] intValue])
             : QStringLiteral("other application's window above button");
    }
    break;
  }
  [windows release];
  if (!ok && why->isEmpty()) *why = QStringLiteral("no window under the button");
  return ok;
}
static QJsonObject commandImpl(QWidget *owner, const QString &op, const QString &token, const QString &expected, QString *error) {
  if (!QaFence::enabled() || QThread::currentThread() != qApp->thread() || QApplication::platformName() != QLatin1String("cocoa")) {
    *error = QStringLiteral("native panel requires QA cocoa GUI thread"); return {};
  }
  if (op == QLatin1String("bind")) {
    if (bound) { *error = QStringLiteral("native panel already bound"); return {}; }
    auto *view = reinterpret_cast<NSView *>(owner->winId());
    NSWindow *parent = view.window;
    NSMutableArray<NSOpenPanel *> *panels = [NSMutableArray array];
    for (NSWindow *window in NSApp.windows) {
      if (![window isKindOfClass:NSOpenPanel.class] || !window.visible) continue;
      NSOpenPanel *panel = (NSOpenPanel *)window;
      // Qt uses either a sheet of this parent or an application-modal panel in this process.
      if (panel.sheetParent == parent || panel.parentWindow == parent || NSApp.modalWindow == panel) [panels addObject:panel];
    }
    if (panels.count != 1) { *error = QStringLiteral("unknown/ambiguous owned native panel"); return {}; }
    bound = [panels.firstObject retain]; number = bound.windowNumber;
    validation = [[Room237QaDirectoryDelegate alloc] init];
    validation->previous = [(id)bound.delegate retain];
    bound.delegate = validation;
    lease = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return {{"token", lease}, {"window", double(number)}, {"pid", double(getpid())}};
  }
  if (token.isEmpty() || token != lease || !present()) { *error = QStringLiteral("stale/unowned native panel token"); return {}; }
  if (op == QLatin1String("cancel")) {
    // Direct cancellation of this retained panel only, even when selection or visibility guard fails.
    [bound cancel:nil]; retire(); return {{"cancelled", true}};
  }
  if (op == QLatin1String("select")) {
    // Test navigation only. The authoritative URLs getter and normal confirm gate remain unchanged.
    if (ticketClock.isValid() || !QaFence::allows(expected) || !QFileInfo(expected).isDir()) {
      *error = QStringLiteral("QA refused native navigation path"); return {};
    }
    bound.directoryURL = [NSURL fileURLWithPath:expected.toNSString() isDirectory:YES];
    bound.nameFieldStringValue = @"";
    // The panel navigates asynchronously (its browser runs in a separate service). Wait, without input, until the
    // authoritative URLs getter reports the requested folder or 2 s pass; read/confirm still recheck everything.
    const QString wanted = QFileInfo(expected).canonicalFilePath();
    QElapsedTimer settle;
    settle.start();
    QString observed;
    bool settled = false;
    while (settle.elapsed() < 2000) {
      NSArray<NSURL *> *urls = present() ? bound.URLs : nil;
      observed = urls.count == 1 && urls.firstObject.isFileURL ? QFileInfo(QString::fromNSString(urls.firstObject.path)).canonicalFilePath() : QString();
      if ((settled = !observed.isEmpty() && observed == wanted)) break;
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
    }
    return {{"navigated", true}, {"settled", settled}, {"settleMs", double(settle.elapsed())},
            {"observed", QaFence::allows(observed) ? observed : QStringLiteral("<outside fence or none>")},
            {"window", double(number)}, {"pid", double(getpid())}};
  }
  if (op == QLatin1String("read")) {
    if (ticketClock.isValid()) { *error = QStringLiteral("native selection ticket already read"); return {}; }
    selected = selection(error);
    identity = panelFileIdentity(selected);
    ticketClock.start();
    if (selected.isEmpty() || identity.isEmpty()) { if (error->isEmpty()) *error = QStringLiteral("selected folder disappeared"); return {}; }
    return {{"path", selected}, {"token", lease}, {"window", double(number)}, {"pid", double(getpid())}};
  }
  if (op == QLatin1String("confirm")) {
    const qint64 age = ticketClock.isValid() ? ticketClock.elapsed() : -1;
    const QString current = selection(error);
    if (!error->isEmpty()) return {};
    if (age < 0 || age > 2000 || selected.isEmpty() || current != selected || identity != panelFileIdentity(current) ||
        !QaFence::allows(expected) || QFileInfo(expected).canonicalFilePath() != current) {
      *error = QStringLiteral("stale/wrong native directory selection"); return {};
    }
    NSButton *button = (NSButton *)bound.defaultButtonCell.controlView;
    // A local default button is clicked when AppKit exposes one; a remote panel (its controls live in the view service,
    // so defaultButtonCell has no local button) is confirmed with its own ok: action. Either way only after the owned,
    // unoccluded, frontmost check, and the delegate's validateURL below remains the last gate on path and identity.
    const bool local = [button isKindOfClass:NSButton.class];
    QString why = local && button.window != bound ? QStringLiteral("button belongs to another window")
                : local && !button.enabled ? QStringLiteral("button disabled") : QString();
    if (why.isEmpty() && !(local ? actionVisible(button, &why) : panelVisible(&why))) {}
    if (!why.isEmpty()) {
      *error = QStringLiteral("native confirmation refused: inactive/occluded/unknown button (") + why + QLatin1Char(')'); return {};
    }
    // Addressed native control action; no CGEvent, keys, mouse, activation or cursor motion.
    if (!validation || !present()) { *error = QStringLiteral("native panel disappeared before action"); return {}; }
    Room237QaDirectoryDelegate *gate = [validation retain];
    bool validated = false, fallback = false;
    QString okFailure;
    @try {
      gate->approvedPath = current;
      gate->approvedIdentity = identity;
      gate->validated = false;
      gate->armed = true;
      if (local) {
        [button performClick:nil];
      } else {
        @try {
          [bound ok:nil];
        } @catch (NSException *exception) {
          okFailure = QString::fromNSString(exception.name) + QStringLiteral(": ") + QString::fromNSString(exception.reason);
        }
        if (!gate->validated && okFailure.isEmpty() == false) {
          // The view service rejected ok:. End this owned panel with OK only after the selection was rechecked in this
          // turn (path and folder identity); acceptChosenRoot then applies the root fence again.
          QString again;
          const QString now = selection(&again);
          if (again.isEmpty() && now == current && panelFileIdentity(now) == identity) {
            if (bound.sheetParent) [bound.sheetParent endSheet:bound returnCode:NSModalResponseOK];
            else if (NSApp.modalWindow == bound) [NSApp stopModalWithCode:NSModalResponseOK];
            fallback = true;
          }
        }
      }
      validated = gate->validated || fallback;
    } @finally {
      gate->armed = false;
      [gate release];
    }
    if (!validated) {
      *error = QStringLiteral("native acceptance URL was not validated") + (okFailure.isEmpty() ? QString() : QStringLiteral(" (ok: ") + okFailure + QLatin1Char(')')); return {};
    }
    retire(); return {{"confirmed", true}, {"path", current}, {"via", local ? QStringLiteral("button") : fallback ? QStringLiteral("end with OK after recheck") : QStringLiteral("panel ok:")}, {"okFailure", okFailure}};
  }
  *error = QStringLiteral("unknown native panel operation"); return {};
}
QJsonObject command(QWidget *owner, const QString &op, const QString &token, const QString &expected, QString *error) {
  @try { return commandImpl(owner, op, token, expected, error); }
  @catch (NSException *exception) {
    *error = QStringLiteral("native panel API failed; scoped cancellation required (") + QString::fromNSString(exception.name) + QStringLiteral(": ") + QString::fromNSString(exception.reason) + QLatin1Char(')'); return {};
  }
}
}
