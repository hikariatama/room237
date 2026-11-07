#include "root_watcher.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
// A directory's modification time moves on every create, delete and rename inside it.
static qint64 stampOf(const QString &path) {
  const QFileInfo info(path);
  return info.exists() ? info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch() : -1;
}
RootWatcher::RootWatcher(QObject *parent) : QObject(parent) {
  pollTimer_.setTimerType(Qt::VeryCoarseTimer);
  pollTimer_.setInterval(1000);
  connect(&pollTimer_, &QTimer::timeout, this, &RootWatcher::poll);
}
RootWatcher::~RootWatcher() { stop(); }
void RootWatcher::start(const QString &root) {
  stop();
  root_ = root;
  stamps_.clear();
  startNative(root);
  stamps_.insert(root, stampOf(root));
  pollTimer_.start();
}
void RootWatcher::stop() {
  pollTimer_.stop();
  stopNative();
}
void RootWatcher::setAlbums(const QStringList &paths, const QString &priority) {
  polled_ = paths;
  priority_ = priority;
  QHash<QString, qint64> next;
  next.insert(root_, stamps_.value(root_, stampOf(root_)));
  for (const QString &path : paths) {
    auto it = stamps_.constFind(path);
    next.insert(path, it != stamps_.cend() ? *it : stampOf(path));
  }
  stamps_ = std::move(next);
#ifndef Q_OS_MACOS
  if (native_) {
    if (const QStringList old = native_->directories(); !old.isEmpty()) native_->removePaths(old);
    QStringList wanted = paths;
    wanted.push_back(root_);
    native_->addPaths(wanted);
  }
#endif
}
void RootWatcher::noteOwnChange(const QString &path) { stamps_.insert(path, stampOf(path)); }
void RootWatcher::poll() {
  // A bounded slice per tick keeps very large libraries cheap; the root and the open album are always checked.
  const auto check = [this](const QString &path) {
    const qint64 stamp = stampOf(path);
    auto it = stamps_.find(path);
    if (it == stamps_.end()) return;
    if (*it != stamp) {
      *it = stamp;
      emit changed(path);
    }
  };
  check(root_);
  if (!priority_.isEmpty()) check(priority_);
  const int slice = qMin(int(polled_.size()), 64);
  for (int i = 0; i < slice; i++) {
    cursor_ = (cursor_ + 1) % polled_.size();
    check(polled_[cursor_]);
  }
}
#ifndef Q_OS_MACOS
void RootWatcher::startNative(const QString &) {
  native_ = new QFileSystemWatcher(this);
  connect(native_, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
    stamps_.insert(path, stampOf(path));
    emit changed(path);
  });
}
void RootWatcher::stopNative() {
  delete native_;
  native_ = nullptr;
}
#endif
