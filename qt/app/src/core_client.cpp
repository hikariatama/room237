#include "core_client.h"
#include "core.h"
#include "fs.h"
#include "host.h"
#include "qatrace.h"
#include "qa_fence.h"
#include "updater.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <mutex>
#include <QTimer>
#include <utility>
CoreClient::CoreClient(QObject *parent) : QObject(parent) {
  // Requests run concurrently; admission is bounded so a scrolling client cannot build an unbounded work queue.
  pool_.setMaxThreadCount(8);
  pool_.setExpiryTimeout(30000);
}
namespace {
// Core calls in flight, for the report at exit.
std::mutex runningMutex;
QHash<qint64, QString> running;
}  // namespace
CoreClient::~CoreClient() {
  core::host::setEventSink({});
  pool_.clear();
  if (!pool_.waitForDone(1500)) {
    QStringList methods;
    {
      std::lock_guard lock(runningMutex);
      methods = running.values();
    }
    fprintf(stderr, "core calls still running at exit: %s\n", qUtf8Printable(methods.join(QStringLiteral(", "))));
  }
#ifdef ROOM237_QA
  if (process_) {
    process_->disconnect(this);
    process_->closeWriteChannel();
    if (!process_->waitForFinished(1500)) process_->kill();
  }
#endif
}
void CoreClient::start(const QStringList &arguments) {
#ifdef ROOM237_QA
  if (QaFence::enabled() && Updater::qaFixtureAllowed()) {
    // The updater checks that it runs from the fixture's namespace.
    const auto fixture = QJsonDocument::fromJson(qgetenv("ROOM237_QA_UPDATE_FIXTURE")).object();
    QDir::setCurrent(fixture.value(QStringLiteral("profile")).toString());
  }
  if (const QString stand = qEnvironmentVariable("ROOM237_QA_CORE_PROCESS"); QaFence::enabled() && !stand.isEmpty()) return startProcess(stand, arguments);
#endif
  QString profile, legacy;
  for (qsizetype i = 0; i + 1 < arguments.size(); ++i) {
    if (arguments[i] == QLatin1String("--profile")) profile = arguments[i + 1];
    else if (arguments[i] == QLatin1String("--legacy-profile")) legacy = arguments[i + 1];
  }
  QPointer<CoreClient> self(this);
  core::host::setEventSink([self](const QString &name, const QJsonValue &payload) {
    QMetaObject::invokeMethod(qApp, [self, name, payload] {
      if (!self) return;
      qaTrace("core-event", name);
      emit self->event(name, payload);
    }, Qt::QueuedConnection);
  });
  started_ = true;
  try {
    core::initialize(profile, legacy);
  } catch (const core::Error &error) {
    failed_ = true;
    lastError_ = error.message();
    qaTrace("core-exit", lastError_);
    fprintf(stderr, "%s\n", qUtf8Printable(lastError_));
    // Nothing will answer: every caller gets an error instead of waiting.
    for (Queued &queued : std::exchange(queued_, {}))
      if (queued.reply) QTimer::singleShot(0, this, [reply = std::move(queued.reply), message = lastError_] { reply({}, message); });
    QTimer::singleShot(0, this, [this] { emit disconnected(lastError_); });
    return;
  }
  // Calls made while the window was being built go out now.
  for (Queued &queued : std::exchange(queued_, {})) run(queued.method, queued.params, std::move(queued.reply));
}
void CoreClient::call(const QString &method, const QJsonObject &params, Reply reply) {
  if (!QaFence::methodAllowed(method) || !QaFence::parametersAllowed(params, method)) {
    qaTrace("qa-fence-refused", method);
    if (reply) QTimer::singleShot(0, this, [reply = std::move(reply)] { reply({}, QStringLiteral("QA fence refused core parameters")); });
    return;
  }
  if (failed_) {
    if (reply) QTimer::singleShot(0, this, [reply = std::move(reply), error = lastError_] { reply({}, error.isEmpty() ? QStringLiteral("core is not running") : error); });
    return;
  }
  if (!started_) {
    queued_.push_back({method, params, std::move(reply)});
    return;
  }
  run(method, params, std::move(reply));
}
void CoreClient::run(const QString &method, const QJsonObject &params, Reply reply) {
  const qint64 id = ++sequence_;
  if (gQaTraceOn) qaTrace("core-call", QStringLiteral("%1 %2").arg(id).arg(method));
#ifdef ROOM237_QA
  if (process_) {
    if (reply) pending_.insert(id, std::move(reply));
    const QJsonObject message{{QStringLiteral("id"), double(id)}, {QStringLiteral("method"), method}, {QStringLiteral("params"), params}};
    process_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    return;
  }
#endif
  QPointer<CoreClient> self(this);
  pool_.start([self, id, method, params, reply = std::move(reply)]() mutable {
    QJsonValue result;
    QString error;
    {
      std::lock_guard lock(runningMutex);
      running.insert(id, method);
    }
    try {
      result = core::dispatch(method, params);
    } catch (const std::exception &e) {
      error = QString::fromUtf8(e.what());
    }
    {
      std::lock_guard lock(runningMutex);
      running.remove(id);
    }
    if (!reply) return;
    QMetaObject::invokeMethod(qApp, [self, id, method, result = std::move(result), error = std::move(error), reply = std::move(reply)] {
      if (!self) return;
      QaSpan span("core-reply", QStringLiteral("%1 %2").arg(id).arg(method));
      if (gQaTraceOn && !error.isEmpty()) qaTrace("core-error", error);
      reply(result, error);
    }, Qt::QueuedConnection);
  });
}
#ifdef ROOM237_QA
void CoreClient::startProcess(const QString &program, const QStringList &arguments) {
  process_ = new QProcess(this);
  process_->setProcessChannelMode(QProcess::SeparateChannels);
  connect(process_, &QProcess::readyReadStandardError, this, [this] {
    const QByteArray text = process_->readAllStandardError();
    lastError_ = QString::fromUtf8(text).trimmed();
    qaTrace("core-stderr", lastError_);
    if (!gQaTraceOn) fwrite(text.constData(), 1, size_t(text.size()), stderr);
  });
  connect(process_, &QProcess::readyReadStandardOutput, this, &CoreClient::readOutput);
  connect(process_, &QProcess::finished, this, [this](int code) {
    qaTrace("core-exit", QString::number(code));
    failed_ = true;
    const auto waiting = std::exchange(pending_, {});
    for (const Reply &reply : waiting) reply({}, QStringLiteral("core exited"));
    emit disconnected(QStringLiteral("core exited %1").arg(code));
  });
  started_ = true;
  process_->start(program, arguments);
  process_->waitForStarted(3000);
  for (Queued &queued : std::exchange(queued_, {})) run(queued.method, queued.params, std::move(queued.reply));
}
void CoreClient::readOutput() {
  buffer_ += process_->readAllStandardOutput();
  qsizetype start = 0;
  for (;;) {
    const qsizetype end = buffer_.indexOf('\n', start);
    if (end < 0) break;
    const QJsonObject message = QJsonDocument::fromJson(QByteArrayView(buffer_).sliced(start, end - start).toByteArray()).object();
    start = end + 1;
    if (const auto name = message.value(QStringLiteral("event")); name.isString()) {
      emit event(name.toString(), message.value(QStringLiteral("payload")));
      continue;
    }
    auto it = pending_.find(qint64(message.value(QStringLiteral("id")).toDouble()));
    if (it == pending_.end()) continue;
    Reply reply = std::move(it.value());
    pending_.erase(it);
    reply(message.value(QStringLiteral("result")), message.value(QStringLiteral("error")).toString());
  }
  if (start) buffer_.remove(0, start);
}
#endif
