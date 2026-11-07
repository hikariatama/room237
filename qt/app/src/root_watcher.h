#pragma once
// Change notification for one library root. Events only say where to look: every wakeup is checked against
// the disk by the core, so a late or lost event costs a little latency and can never lose a change.
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QHash>
#include <QStringList>
#include <atomic>
class QFileSystemWatcher;
class RootWatcher : public QObject {
  Q_OBJECT
public:
  explicit RootWatcher(QObject *parent = nullptr);
  ~RootWatcher() override;
  void start(const QString &root);
  void stop();
  // Directories are also polled, so a delayed system notification cannot delay a visible change for long.
  void setAlbums(const QStringList &absolutePaths, const QString &priority);
  void setPriority(const QString &priorityPath) { priority_ = priorityPath; }
  void noteOwnChange(const QString &absolutePath);
signals:
  void changed(const QString &directory);  // Directory holding a visible entry that changed.
private:
  void poll();
  void startNative(const QString &root);
  void stopNative();
  QString root_;
  QStringList polled_;
  QString priority_;
  QHash<QString, qint64> stamps_;
  QTimer pollTimer_;
  int cursor_ = 0;
#ifdef Q_OS_MACOS
  static bool isDerivedPath(const char *path);
  QByteArray rootBytes_;
  static void callback(const void *stream, void *self, size_t count, void *paths, const unsigned *flags, const unsigned long long *ids);
  void *stream_ = nullptr;
  void *queue_ = nullptr;
#else
  QFileSystemWatcher *native_ = nullptr;
#endif
};
