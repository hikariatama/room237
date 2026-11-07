#pragma once
// Everything that differs per operating system.
#include <QImage>
#include <QString>
class QWidget;
class QWindow;
namespace Platform {
QString fileManagerName();
QByteArray fileManagerIcon();
void revealInFileManager(const QString &path);
bool canProtectContent();
void setContentProtected(QWidget *window, bool enabled);
bool reduceMotion();
// Native title bar over the content on macOS; regular frame elsewhere.
void configureWindow(QWidget *window);
int titleBarInset();  // Height the traffic lights occupy inside the content, 0 when the frame is outside it.
void activateApplication();
void roundSurface(QWindow *surface, int radius);  // Rounds a native child surface where a region mask stays jagged or has no effect.
void layoutWindowControls(QWidget *window);  // Positions the traffic lights inside the content.
void setDockVisibleProgress(double fraction);
QString defaultProfileDirectory();
QString legacyProfileDirectory();
QImage decodeImageNative(const QString &path, int maxPixelSize = 0, QSize *fullSize = nullptr);  // HEIC, AVIF: ImageIO on macOS, null elsewhere.
QString platformKey();  // darwin-aarch64, windows-x86_64, linux-x86_64-appimage ...
// Path of what an update replaces, the probe inside it and how to start it again.
struct InstallInfo {
  QString install, probe, kind, unavailableReason;
  QStringList relaunch;
};
InstallInfo installInfo();
void lowerThreadPriority();
// A closed window-sized view has just freed large blocks; hands them back instead of keeping them in the allocator.
void releaseFreeMemory();
}
