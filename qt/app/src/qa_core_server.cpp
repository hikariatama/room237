// QA builds only: "Room237 --qa-core-server --profile DIR [--legacy-profile DIR]" serves the core over JSON lines on
// stdin/stdout, the protocol of the earlier separate core process. Suites put a recording or delaying stand-in between
// the window and this server (ROOM237_QA_CORE_PROCESS).
#include "qa_core_server.h"
#include "core.h"
#include "fs.h"
#include "host.h"
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {
std::mutex outputMutex;
void writeLine(const QJsonObject &message) {
  const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  std::lock_guard lock(outputMutex);
  fwrite(line.constData(), 1, size_t(line.size()), stdout);
  fflush(stdout);
}
}  // namespace

bool isQaCoreServer(int argc, char **argv) { return argc > 1 && qstrcmp(argv[1], "--qa-core-server") == 0; }

int runQaCoreServer(int argc, char **argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  QString profile, legacy;
  const QStringList args = QCoreApplication::arguments();
  for (qsizetype i = 1; i + 1 < args.size(); ++i) {
    if (args[i] == QLatin1String("--profile")) profile = args[i + 1];
    if (args[i] == QLatin1String("--legacy-profile")) legacy = args[i + 1];
  }
  try {
    core::initialize(profile, legacy);
  } catch (const core::Error &error) {
    fprintf(stderr, "%s\n", qUtf8Printable(error.message()));
    return 1;
  }
  core::host::setEventSink([](const QString &name, const QJsonValue &payload) { writeLine({{QStringLiteral("event"), name}, {QStringLiteral("payload"), payload}}); });
  QThreadPool pool;
  pool.setMaxThreadCount(8);
  std::thread reader([&] {
    std::string line;
    while (std::getline(std::cin, line)) {
      const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
      pool.start([request] {
        const QJsonValue id = request.value(QStringLiteral("id"));
        try {
          writeLine({{QStringLiteral("id"), id}, {QStringLiteral("result"), core::dispatch(request.value(QStringLiteral("method")).toString(), request.value(QStringLiteral("params")).toObject())}});
        } catch (const std::exception &error) {
          writeLine({{QStringLiteral("id"), id}, {QStringLiteral("error"), QString::fromUtf8(error.what())}});
        }
      });
    }
    pool.waitForDone();
    QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
  });
  const int code = app.exec();
  reader.join();
  core::host::setEventSink({});
  return code;
}
