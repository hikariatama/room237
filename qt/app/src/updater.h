#pragma once
// Signed update flow: fetch manifest and signature, let the core verify them, download with progress,
// let the core verify and unpack, then hand over to the detached helper that swaps and can roll back.
#include "core_client.h"
#include <QFile>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
class QNetworkReply;
class Updater : public QObject {
  Q_OBJECT
public:
  Updater(CoreClient *core, const QString &profile, QObject *parent = nullptr);
#ifdef ROOM237_QA
  // A QA run updates only between the fixture's fenced installations (ROOM237_QA_UPDATE_FIXTURE).
  static bool qaFixtureAllowed();
  static bool qaCoreAllowed(const QString &method, const QJsonObject &params);
#else
  static constexpr bool qaFixtureAllowed() { return false; }
#endif
  QString previousResult() const { return previousResult_; }
  void start();
  void setAutomatic(bool automatic);
  void checkNow(bool interactive);
  void install();
  void cancel();
  void markHealthyIfUpdating();
  void reportPreviousResult();
  QString state() const { return state_; }
  QString availableVersion() const { return version_; }
signals:
  void message(int kind, QString text);  // 0 success, 1 error, 2 info
  void available(QString version);
  void none();
  void progress(double fraction);
  void downloading();
  void failed(QString text);
  void readyToRestart();
  void manualOnly(QString reason, QString version);
private:
  void fetchSignature(const QByteArray &manifest);
  void verify(const QByteArray &manifest, const QByteArray &signature);
  void downloadPayload();
  void stagePayload();
  void fail(const QString &text);
  QString feedUrl() const;
  QNetworkAccessManager *network();
  CoreClient *core_;
  QString previousResult_;
  QString profile_, state_ = QStringLiteral("idle"), version_, url_, sha256_, kind_, payloadPath_;
  qint64 size_ = 0;
  bool interactive_ = false, automatic_ = true;
  QNetworkAccessManager *network_ = nullptr;
  QPointer<QNetworkReply> reply_;
  QFile *file_ = nullptr;
  QTimer periodic_;
};
