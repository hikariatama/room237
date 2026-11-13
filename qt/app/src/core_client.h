#pragma once
// Calls into the library core on worker threads; replies and events arrive on the interface thread.
// QA builds can talk to a separate process instead (ROOM237_QA_CORE_PROCESS), over the JSON lines protocol that
// "Room237 --qa-core-server" serves, so suites can delay, reorder or forge answers.
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QThreadPool>
#include <functional>
#ifdef ROOM237_QA
#include <QProcess>
#endif
class CoreClient : public QObject {
  Q_OBJECT
public:
  using Reply = std::function<void(const QJsonValue &result, const QString &error)>;
  explicit CoreClient(QObject *parent = nullptr);
  ~CoreClient() override;
  // Arguments as the earlier core process took them: --profile DIR [--legacy-profile DIR].
  void start(const QStringList &arguments);
  void call(const QString &method, const QJsonObject &params = {}, Reply reply = {});
  QString lastError() const { return lastError_; }
signals:
  void event(const QString &name, const QJsonValue &payload);
  void disconnected(const QString &message);
private:
  struct Queued {
    QString method;
    QJsonObject params;
    Reply reply;
  };
  void run(const QString &method, const QJsonObject &params, Reply reply);
  QThreadPool pool_;
  QString lastError_;
  bool started_ = false, failed_ = false;
  qint64 sequence_ = 0;
  QList<Queued> queued_;  // Requests made before start().
#ifdef ROOM237_QA
  void startProcess(const QString &program, const QStringList &arguments);
  void readOutput();
  QProcess *process_ = nullptr;
  QByteArray buffer_;
  QHash<qint64, Reply> pending_;
#endif
};
