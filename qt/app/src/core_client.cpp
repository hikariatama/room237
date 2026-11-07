#include "core_client.h"
#include "qatrace.h"
#include "qa_fence.h"
#include "updater.h"
#include <QJsonDocument>
#include <QTimer>
#include <utility>
CoreClient::CoreClient(QObject *parent) : QObject(parent) {
  // A GUI process may have no usable stderr to hand down, so the core's diagnostics come through a pipe.
  process_.setProcessChannelMode(QProcess::SeparateChannels);
  connect(&process_, &QProcess::readyReadStandardError, this, [this] {
    const QByteArray text = process_.readAllStandardError();
    lastError_ = QString::fromUtf8(text).trimmed();
    qaTrace("core-stderr", lastError_);
    if (!gQaTraceOn) fwrite(text.constData(), 1, size_t(text.size()), stderr);
  });
  connect(&process_, &QProcess::readyReadStandardOutput, this, &CoreClient::readOutput);
  connect(&process_, &QProcess::finished, this, [this](int code) {
    qaTrace("core-exit", QString::number(code));
    // Nothing will answer anymore: every waiting caller gets an error instead of hanging.
    const auto waiting = std::exchange(pending_, {});
    for (const Reply &reply : waiting) reply({}, QStringLiteral("core exited"));
    emit disconnected(QStringLiteral("core exited %1").arg(code));
  });
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) { qaTrace("core-process-error", QStringLiteral("%1 %2").arg(int(error)).arg(process_.errorString())); });
}
CoreClient::~CoreClient() {
  process_.disconnect(this);
  process_.closeWriteChannel();
  if (!process_.waitForFinished(1500)) process_.kill();
}
void CoreClient::start(const QString &binary, const QStringList &arguments) {
#ifdef ROOM237_QA
  if (QaFence::enabled() && Updater::qaFixtureAllowed()) {
    const auto fixture = QJsonDocument::fromJson(qgetenv("ROOM237_QA_UPDATE_FIXTURE")).object();
    process_.setWorkingDirectory(fixture.value(QStringLiteral("profile")).toString());
  }
#endif
  started_ = true;
  process_.start(binary, arguments);
  process_.waitForStarted(3000);
  // Calls made while the window was being built go out now; written earlier they would be lost unanswered.
  if (!queued_.isEmpty()) process_.write(std::exchange(queued_, {}));
}
void CoreClient::call(const QString &method, const QJsonObject &params, Reply reply) {
  if (!QaFence::methodAllowed(method) || !QaFence::parametersAllowed(params, method)) {
    qaTrace("qa-fence-refused", method);
    if (reply) QTimer::singleShot(0, this, [reply = std::move(reply)] { reply({}, QStringLiteral("QA fence refused core parameters")); });
    return;
  }
  // A call after the core has gone is answered at once, like the ones that were pending when it exited.
  if (process_.state() == QProcess::NotRunning && started_) {
    if (reply) {
      const QString error = lastError().isEmpty() ? QStringLiteral("core is not running") : lastError();
      QTimer::singleShot(0, this, [reply = std::move(reply), error] { reply({}, error); });
    }
    return;
  }
  const qint64 id = ++sequence_;
  if (reply) pending_.insert(id, std::move(reply));
  QJsonObject message{{QStringLiteral("id"), double(id)}, {QStringLiteral("method"), method}, {QStringLiteral("params"), params}};
  const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  if (gQaTraceOn) {
    traceNames_.insert(id, method);
    qaTrace("core-call", QStringLiteral("%1 %2 bytes=%3").arg(id).arg(method).arg(line.size()));
  }
  if (!started_) queued_ += line;
  else process_.write(line);
}
void CoreClient::readOutput() {
  buffer_ += process_.readAllStandardOutput();
  qsizetype start = 0;
  for (;;) {
    const qsizetype end = buffer_.indexOf('\n', start);
    if (end < 0) break;
    const qsizetype lineBytes = end - start;
    const QJsonObject message = QJsonDocument::fromJson(QByteArrayView(buffer_).sliced(start, end - start).toByteArray()).object();
    start = end + 1;
    if (const auto name = message.value(QStringLiteral("event")); name.isString()) {
      qaTrace("core-event", QStringLiteral("%1 bytes=%2").arg(name.toString()).arg(lineBytes));
      emit event(name.toString(), message.value(QStringLiteral("payload")));
      continue;
    }
    const qint64 id = qint64(message.value(QStringLiteral("id")).toDouble());
    auto it = pending_.find(id);
    if (it == pending_.end()) continue;
    Reply reply = std::move(it.value());
    pending_.erase(it);
    QaSpan span("core-reply", QStringLiteral("%1 %2 bytes=%3").arg(id).arg(traceNames_.take(id)).arg(lineBytes));
    if (gQaTraceOn && message.contains(QStringLiteral("error"))) qaTrace("core-error", message.value(QStringLiteral("error")).toString());
    reply(message.value(QStringLiteral("result")), message.value(QStringLiteral("error")).toString());
  }
  if (start) buffer_.remove(0, start);
}
