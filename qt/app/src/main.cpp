#include "accessibility.h"
#include "platform.h"
#ifdef ROOM237_QA
#include "qa.h"
#include "qa_core_server.h"
#include "qa_fence.h"
#include "qa_scroll_log.h"
#endif
#include "qatrace.h"
#include "theme.h"
#include "window.h"
#include <cstdio>
#include "widgets.h"
#include <QColorSpace>
#include <QSurfaceFormat>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QIcon>
// Usage: room237 [--root DIR] [--profile DIR] [--legacy-profile DIR].
int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (qstrcmp(argv[i], "--version") == 0) {
      fprintf(stdout, "%s\n", ROOM237_VERSION);
      return 0;
    }
    // Tests check where the default locations resolve under a synthetic home before anything can run there.
    if (qstrcmp(argv[i], "--print-default-paths") == 0) {
      fprintf(stdout, "profile=%s\nlegacy=%s\n", qPrintable(Platform::defaultProfileDirectory()), qPrintable(Platform::legacyProfileDirectory()));
      return 0;
    }
  }
#ifdef ROOM237_QA
  if (isQaCoreServer(argc, argv)) return runQaCoreServer(argc, argv);
#endif
  installAccessibilityFactory();
  // Pixels are sRGB; the window system converts them for a wide-gamut display.
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setColorSpace(QColorSpace::SRgb);
  QSurfaceFormat::setDefaultFormat(format);
  QApplication app(argc, argv);
  qaTraceInit();
  QApplication::setApplicationName(QStringLiteral("Room237"));
  QApplication::setApplicationVersion(QStringLiteral(ROOM237_VERSION));
  QApplication::setOrganizationDomain(QStringLiteral("dgazizullin.dev"));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/icon.png")));
  QApplication::setDesktopFileName(QStringLiteral("dev.dgazizullin.room237"));
  Theme::applyApplicationFont();
  installFocusRing();
  Theme::applyApplicationStyle();
  QString root, profile, legacy;
  const QStringList args = QCoreApplication::arguments();
  for (int i = 1; i + 1 < args.size(); i++) {
    if (args[i] == QLatin1String("--root")) root = args[i + 1];
    if (args[i] == QLatin1String("--profile")) profile = args[i + 1];
    if (args[i] == QLatin1String("--legacy-profile")) legacy = args[i + 1];
  }
#ifdef ROOM237_QA
  // QA runs must point inside a disposable fence.
  const QByteArray port = qgetenv("ROOM237_QA_PORT");
  const QString fence = qEnvironmentVariable("ROOM237_QA_FENCE");
  const bool qa = QaFence::enabled();
  if (qa && (!(QaFence::inside(root, fence) && QFileInfo(root).isDir()) || !QaFence::inside(profile, fence, true) || (!legacy.isEmpty() && !QaFence::inside(legacy, fence)))) {
    fprintf(stderr, "%s\n", "QA needs an explicit --root and --profile inside the disposable fence");
    return 2;
  }
  if (qa && (!QaFence::profileAllowed(profile) || (!legacy.isEmpty() && !QaFence::profileAllowed(legacy, true)))) {
    fprintf(stderr, "%s\n", "QA fence refused saved profile roots before core startup");
    return 2;
  }
#endif
  // Looking for the old installation is the behaviour of a normal start only. An explicit profile is an isolated
  // installation; it reads an old profile only when it was named on the command line.
  if (profile.isEmpty()) {
    profile = Platform::defaultProfileDirectory();
    legacy = Platform::legacyProfileDirectory();
    qaTrace("legacy-discovery", legacy);
  }
  QDir().mkpath(profile);
  MainWindow window(profile, root, legacy);
  window.show();
#ifdef ROOM237_QA
  if (qa) new QaDriver(&window, quint16(port.toUShort()), fence);
  if (qEnvironmentVariableIsSet("ROOM237_SCROLL_LOG")) installScrollLog(&window, qEnvironmentVariable("ROOM237_SCROLL_LOG"));
#endif
  return app.exec();
}
