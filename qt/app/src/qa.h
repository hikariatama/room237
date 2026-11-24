#pragma once
#include <QWidget>
// QA-only controller client; inactive unless ROOM237_QA_PORT is set.
#include "window.h"
#include <QTcpSocket>
#include <QMimeData>
#include <QTimer>
class QaDriver : public QObject {
  Q_OBJECT
public:
  QaDriver(MainWindow *window, quint16 port, const QString &fence);
protected:
  bool eventFilter(QObject *object, QEvent *event) override;
private:
  void handle(const QJsonObject &c);
  void reply(const QJsonValue &id, const QJsonValue &result);
  void fail(const QJsonValue &id, const QString &error);
  void until(std::function<bool()> test, int timeoutMs, std::function<void(bool)> done);
  QJsonObject state();
  QJsonObject recordingToJson(FrameRecording rec, double durationMs);
  MainWindow *w_;
  QString fence_;
  QWidget *ownWindow_ = nullptr;
  bool nativeDirectoryPending_ = false;
  QString nativeDirectorySession_;
  QTcpSocket socket_;
  QByteArray buffer_;
  QVector<MediaItem> pendingEntries_;
  QStringList pendingPaths_;
  bool recordingDup_ = false;
  QElapsedTimer recordClock_, heartbeatClock_;
  QTimer heartbeat_;
  // Diagnostics: paint events per widget class and model signals since the last "diag" reset; frame capture.
  QHash<QString, int> paints_, modelEvents_;
  QTimer film_;
  QString filmDir_;
  int filmIndex_ = 0;
  double filmScale_ = 0.5;
  QElapsedTimer filmClock_;
  void countModel(const char *name, const QString &detail = {});
  void sendDrop(QWidget *target, QPoint at, QMimeData *mime, Qt::DropActions actions, int holdMs, std::function<void()> done);
  GalleryView::Visible anyVisible(QVector<QPair<QString, bool>> *keys = nullptr);
};
