#include "platform.h"
#include "theme.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUrl>
#include <QWidget>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#elif !defined(Q_OS_MACOS)
#include <QDBusConnection>
#include <QDBusMessage>
#endif
namespace Platform {
#ifndef Q_OS_MACOS
QString fileManagerName() {
#ifdef Q_OS_WIN
  return QStringLiteral("Explorer");
#else
  const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
  if (desktop.contains(QLatin1String("kde"))) return QStringLiteral("Dolphin");
  if (desktop.contains(QLatin1String("gnome")) || desktop.contains(QLatin1String("ubuntu"))) return QStringLiteral("Files");
  if (desktop.contains(QLatin1String("pantheon"))) return QStringLiteral("Files");
  if (desktop.contains(QLatin1String("xfce"))) return QStringLiteral("Thunar");
  return QStringLiteral("file manager");
#endif
}
void revealInFileManager(const QString &path) {
#ifdef Q_OS_WIN
  QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
  // Standard file manager interface; a plain folder open when no service answers.
  QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"), QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
  call.setArguments({QStringList{QUrl::fromLocalFile(path).toString()}, QString()});
  if (QDBusConnection::sessionBus().call(call, QDBus::Block, 1500).type() == QDBusMessage::ErrorMessage) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
#endif
}
bool reduceMotion() {
#ifdef Q_OS_WIN
  BOOL enabled = TRUE;
  return SystemParametersInfoW(0x1042 /* SPI_GETCLIENTAREAANIMATION */, 0, &enabled, 0) && !enabled;
#else
  static const bool reduced = [] {
    QProcess process;
    process.start(QStringLiteral("gsettings"), {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("enable-animations")});
    return process.waitForFinished(300) && process.readAllStandardOutput().trimmed() == "false";
  }();
  return reduced;
#endif
}
bool canProtectContent() {
#ifdef Q_OS_WIN
  return true;
#else
  return false;
#endif
}
void setContentProtected(QWidget *window, bool enabled) {
#ifdef Q_OS_WIN
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
  SetWindowDisplayAffinity(reinterpret_cast<HWND>(window->winId()), enabled ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
#else
  Q_UNUSED(window);
  Q_UNUSED(enabled);
#endif
}
void configureWindow(QWidget *) {}
int titleBarInset() { return 0; }
void layoutWindowControls(QWidget *) {}
QByteArray fileManagerIcon() { return "folder"; }
void activateApplication() {}
void roundSurface(QWindow *, int) {}
void setDockVisibleProgress(double) {}
QImage decodeImageNative(const QString &, int, QSize *) { return {}; }
void lowerThreadPriority() { QThread::currentThread()->setPriority(QThread::LowPriority); }
void releaseFreeMemory() {
#if defined(__GLIBC__)
  malloc_trim(0);
#endif
}
#endif
QString defaultProfileDirectory() {
#ifdef Q_OS_MACOS
  return QDir::home().filePath(QStringLiteral("Library/Application Support/dev.dgazizullin.room237.qt"));
#elif defined(Q_OS_WIN)
  return QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("dev.dgazizullin.room237.qt"));
#else
  const QString base = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::home().filePath(QStringLiteral(".config")));
  return QDir(base).filePath(QStringLiteral("dev.dgazizullin.room237.qt"));
#endif
}
QString legacyProfileDirectory() {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
  QString directory = defaultProfileDirectory();
  directory.chop(3);  // The Tauri build used the same name without ".qt".
  return directory;
#else
  // tauri-plugin-store resolves state.json against Tauri's app data dir (dirs::data_dir), not the config dir.
  const QString base = qEnvironmentVariable("XDG_DATA_HOME", QDir::home().filePath(QStringLiteral(".local/share")));
  return QDir(base).filePath(QStringLiteral("dev.dgazizullin.room237"));
#endif
}
QString platformKey() {
  const QString arch = QSysInfo::buildCpuArchitecture() == QLatin1String("arm64") ? QStringLiteral("aarch64") : QStringLiteral("x86_64");
#ifdef Q_OS_MACOS
  return QStringLiteral("darwin-") + arch;
#elif defined(Q_OS_WIN)
  return QStringLiteral("windows-") + arch;
#else
  return (qEnvironmentVariableIsSet("APPIMAGE") ? QStringLiteral("linux-appimage-") : QStringLiteral("linux-")) + arch;
#endif
}
InstallInfo installInfo() {
  InstallInfo info;
  const QString app = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
  QDir bundle(app);
  bundle.cdUp();
  bundle.cdUp();
  info.install = bundle.absolutePath();
  info.probe = QStringLiteral("Contents/MacOS/room237-core");
  info.kind = QStringLiteral("tar.gz");
  info.relaunch = {QStringLiteral("/usr/bin/open"), QStringLiteral("-n"), info.install};
  // Isolated update tests need the new process to inherit their environment.
  if (qEnvironmentVariableIsSet("ROOM237_UPDATE_DIRECT_RELAUNCH")) info.relaunch = {QCoreApplication::applicationFilePath()};
  if (info.install.contains(QLatin1String("/AppTranslocation/")) || info.install.startsWith(QLatin1String("/Volumes/"))) info.unavailableReason = QStringLiteral("translocated");
#elif defined(Q_OS_WIN)
  info.install = app;
  info.probe = QStringLiteral("room237-core.exe");
  info.kind = QStringLiteral("tar.gz");
  info.relaunch = {QDir(app).filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName())};
#else
  if (const QString image = qEnvironmentVariable("APPIMAGE"); !image.isEmpty()) {
    info.install = image;
    info.kind = QStringLiteral("appimage");
    info.relaunch = {image};
  } else {
    info.install = QFileInfo(app).fileName() == QLatin1String("bin") ? QFileInfo(app).absolutePath() : app;
    info.probe = QDir(info.install).relativeFilePath(QDir(app).filePath(QStringLiteral("room237-core")));
    info.kind = QStringLiteral("tar.gz");
    info.relaunch = {QCoreApplication::applicationFilePath()};
    if (info.install.startsWith(QLatin1String("/usr/")) || info.install.startsWith(QLatin1String("/opt/"))) info.unavailableReason = QStringLiteral("packaged");
  }
#endif
  if (info.unavailableReason.isEmpty() && !QFileInfo(QFileInfo(info.install).absolutePath()).isWritable()) info.unavailableReason = QStringLiteral("readonly");
  return info;
}
}
