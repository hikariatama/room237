#pragma once
// Line-JSON client for the shared room237-core process. Images never cross this pipe.
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QProcess>
#include <functional>
class CoreClient : public QObject {
  Q_OBJECT
public:
  using Reply = std::function<void(const QJsonValue &result, const QString &error)>;
  explicit CoreClient(QObject *parent = nullptr);
  ~CoreClient() override;
  void start(const QString &binary, const QStringList &arguments);
  void call(const QString &method, const QJsonObject &params = {}, Reply reply = {});
  qint64 processId() const { return process_.processId(); }
  QString lastError() const { return lastError_; }
signals:
  void event(const QString &name, const QJsonValue &payload);
  void disconnected(const QString &message);
private:
  void readOutput();
  QProcess process_;
  QByteArray buffer_;
  QString lastError_;
  qint64 sequence_ = 0;
  bool started_ = false;
  QByteArray queued_;  // Requests made before start().
  QHash<qint64, Reply> pending_;
  QHash<qint64, QString> traceNames_;
};
