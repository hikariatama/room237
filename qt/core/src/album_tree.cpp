#include "album_tree.h"

#include "settings.h"

#include <QHash>
#include <mutex>
#include <thread>

namespace core::album_tree {

namespace {
const QString kLock = QStringLiteral(".room237-tree.lock");
std::mutex readersMutex;
QHash<QString, std::weak_ptr<FileLock>> readerCache;

struct Identity {
  QString requested, canonical, id;
};

std::set<QString> discover(const QStringList &albums, const QString *exclusive, std::vector<Identity> *identities) {
  std::set<QString> locks;
  for (const QString &requested : albums) {
    const QString album = canonical(requested);
    if (identities) identities->push_back({requested, album, directoryId(album)});
    for (const QString &ancestor : path::ancestors(album)) {
      const QString lock = path::join(ancestor, kLock);
      const FileInfo info = lstat(lock);
      if (info.exists) {
        if (!info.isFile) fail(QStringLiteral("Album tree lock is not a regular file"));
        locks.insert(lock);
      } else if (exclusive && ancestor == *exclusive) {
        locks.insert(lock);
      }
    }
  }
  return locks;
}

std::shared_ptr<FileLock> tryShared(const QString &lock) {
  {
    std::lock_guard guard(readersMutex);
    if (auto cached = readerCache.value(lock).lock(); cached && cached->ownsPath(lock)) return cached;
  }
  std::unique_ptr<FileLock> lease = FileLock::tryAcquireShared(lock);
  if (!lease) return nullptr;
  std::lock_guard guard(readersMutex);
  // Another thread can publish its shared lease during the system call.
  if (auto existing = readerCache.value(lock).lock(); existing && existing->ownsPath(lock)) return existing;
  std::shared_ptr<FileLock> shared(std::move(lease));
  readerCache.insert(lock, shared);
  return shared;
}
}  // namespace

TreeLease::~TreeLease() {
  readers_.clear();
  std::lock_guard guard(readersMutex);
  for (auto it = readerCache.begin(); it != readerCache.end();) {
    if (it.value().expired()) it = readerCache.erase(it);
    else ++it;
  }
}

void TreeLease::ensureCurrent(const QStringList &albums) const {
  const std::set<QString> markers = discover(albums, nullptr, nullptr);
  for (const QString &marker : markers)
    if (!markers_.count(marker)) fail(QStringLiteral("Album tree lease appeared during admission; retry operation"));
  for (const QString &marker : markers) {
    bool owned = false;
    for (const auto &[path, lease] : readers_)
      if (path == marker && lease->ownsPath(marker)) owned = true;
    if (writer_ && writer_->first == marker && writer_->second->ownsPath(marker)) owned = true;
    if (!owned) fail(QStringLiteral("Album tree lock changed during admission"));
  }
}

std::unique_ptr<TreeLease> acquire(const QStringList &albums, const QString *exclusive, bool nonblocking) {
  std::vector<Identity> identities;
  const std::set<QString> markers = discover(albums, exclusive, &identities);
  const auto timeout = std::chrono::seconds(readSettings().ffmpeg.timeoutSecs);
  const auto started = std::chrono::steady_clock::now();
  auto lease = std::make_unique<TreeLease>();
  lease->markers_ = markers;
  // Ancestors precede descendants, and multi-library moves use one order.
  std::vector<QString> locks(markers.begin(), markers.end());
  std::sort(locks.begin(), locks.end(), [](const QString &a, const QString &b) {
    const int ca = path::componentCount(a), cb = path::componentCount(b);
    return ca != cb ? ca < cb : a < b;
  });
  for (const QString &lock : locks) {
    if (exclusive && path::parent(lock) == *exclusive) {
      lease->writer_.emplace(lock, FileLock::acquire(lock, std::chrono::duration_cast<std::chrono::milliseconds>(timeout), std::chrono::milliseconds(5)));
      continue;
    }
    for (;;) {
      if (auto shared = tryShared(lock)) {
        lease->readers_.emplace_back(lock, std::move(shared));
        break;
      }
      if (nonblocking) return nullptr;
      if (std::chrono::steady_clock::now() - started >= timeout) fail(QStringLiteral("Album tree lease timed out"));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  for (const Identity &identity : identities) {
    if (tryCanonical(identity.requested) != identity.canonical || directoryId(identity.canonical) != identity.id) fail(QStringLiteral("Album changed while waiting for tree lease"));
  }
  return lease;
}

std::unique_ptr<TreeLease> read(const QStringList &albums) {
  auto lease = acquire(albums, nullptr, false);
  if (!lease) fail(QStringLiteral("Album tree is busy"));
  return lease;
}
std::unique_ptr<TreeLease> tryRead(const QStringList &albums) { return acquire(albums, nullptr, true); }
std::unique_ptr<TreeLease> write(const QString &root) {
  const QString real = canonical(root);
  auto lease = acquire({real}, &real, false);
  if (!lease) fail(QStringLiteral("Album tree is busy"));
  return lease;
}
void initialize(const QString &root) {
  const QString real = canonical(root);
  const QString identity = directoryId(real);
  auto lease = FileLock::tryAcquireShared(path::join(real, kLock));
  if (directoryId(real) != identity) fail(QStringLiteral("Library root changed"));
}

}  // namespace core::album_tree
