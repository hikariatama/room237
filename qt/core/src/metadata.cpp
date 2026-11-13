#include "metadata.h"

#include "album_db.h"
#include "catalog.h"
#include "constants.h"
#include "json.h"
#include "media_delete.h"
#include "media_probe.h"
#include "native_image.h"
#include "thumb.h"
#include "util.h"
#include "vault.h"
#include "xattr.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimeZone>
#include <QSet>
#include <condition_variable>
#include <deque>
#include <future>
#include <thread>

#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif

namespace core::metadata {

namespace {
const QString kAlbumMetaFile = QStringLiteral("album.sqlite");
constexpr const char *kEmbeddedXattr = "user.room237.shoot_ts";
constexpr auto kLeaseTimeout = std::chrono::seconds(30);
thread_local bool backgroundWrites = false;

QString hex(quint64 value) { return QString::number(qulonglong(value), 16); }
QString hexSigned(qint64 value) { return QString::number(qulonglong(quint64(value)), 16); }
QString nameOf(const QString &path) {
  const QString name = path::fileName(path);
  if (name.isEmpty()) fail(QStringLiteral("Invalid filename"));
  return name;
}
QString dirOf(const QString &path) {
  const QString dir = path::parent(path);
  if (dir.isEmpty() && !path::isAbsolute(path)) return dir;
  if (dir.isEmpty()) fail(QStringLiteral("Invalid path"));
  return dir;
}
}  // namespace

QString metaDir(const QString &dir) { return path::join(dir, kMetaDir); }
QString albumMetaPath(const QString &dir) { return path::join(metaDir(dir), kAlbumMetaFile); }
bool supportedTimestamp(qint64 timestamp) { return timestamp >= -(qint64(1) << 40) && timestamp < (qint64(1) << 40); }

// --- Revisions ---------------------------------------------------------------------------------------------------

std::optional<QString> sourceRevisionFromInfo(const FileInfo &info) {
  if (!info.exists) return std::nullopt;
  const quint64 nanos = info.mtimeNs > 0 ? quint64(info.mtimeNs) : 0;
#ifdef Q_OS_WIN
  const quint64 identity = 0;
#else
  const quint64 identity = info.ino;
#endif
  return QStringLiteral("%1-%2-%3").arg(hex(nanos), hex(info.size), hex(identity));
}
std::optional<QString> sourceRevision(const QString &path) { return sourceRevisionFromInfo(stat(path)); }

std::pair<QString, QString> mediaRevisionParts(const QString &path) {
  QString parent = path::parent(path);
  if (parent.isEmpty()) parent = QStringLiteral(".");
  std::optional<QString> preview;
  try {
    preview = thumb::thumbPath(path, path::join(parent, thumb::kThumbDir));
  } catch (const Error &) {
  }
  return {sourceRevision(path).value_or(QStringLiteral("missing")), preview ? sourceRevision(*preview).value_or(QStringLiteral("missing")) : QStringLiteral("missing")};
}
QString mediaRevision(const QString &path) {
  const auto [source, preview] = mediaRevisionParts(path);
  return source + QLatin1Char('-') + preview;
}

std::vector<ObservedMedia> ObservedMedia::fromJson(const QJsonValue &value) {
  if (!value.isArray()) fail(QStringLiteral("invalid type: expected a sequence"));
  std::vector<ObservedMedia> out;
  for (const QJsonValue &item : value.toArray()) {
    const QJsonObject o = item.toObject();
    out.push_back({json::stringField(o.value(QLatin1String("path")), "path"), json::stringField(o.value(QLatin1String("revision")), "revision")});
  }
  return out;
}

void validateMediaRevisions(const std::vector<ObservedMedia> &observed) {
  bool stale = observed.empty();
  for (const ObservedMedia &item : observed)
    if (item.revision.isEmpty() || !isFile(item.path) || mediaRevision(item.path) != item.revision) stale = true;
  if (stale) fail(QStringLiteral("Media changed; refresh duplicates before deleting."));
}

void deleteMediaIfUnchanged(const QString &path, const QString &revision, const std::vector<ObservedMedia> &observed) { media_delete::deleteIfUnchanged(path, revision, observed); }

// --- Embedded timestamp ------------------------------------------------------------------------------------------

static std::optional<QString> embeddedTimestampRevision(const QString &path) {
#ifndef Q_OS_WIN
  // Xattrs follow the directory entry's object, including symlinks. Their changes update ctime without
  // invalidating visual content/hash revisions.
  const FileInfo info = lstat(path);
  if (!info.exists) return std::nullopt;
  return QStringLiteral("%1-%2-%3-%4").arg(hex(info.dev), hex(info.ino), hexSigned(info.ctimeSec), hexSigned(info.ctimeNsec));
#else
  Q_UNUSED(path)
  return std::nullopt;
#endif
}

// The value an encrypted file carries is sealed: a manual date is as private as the photograph.
static QByteArray timestampValue(const QString &path, qint64 timestamp) {
  const QByteArray plain = QByteArray::number(timestamp);
  if (const auto keys = vault::keysFor(path); keys && vault::isEncryptedFile(path)) return vault::sealAttribute(*keys, path, plain);
  return plain;
}

std::optional<qint64> readEmbeddedTimestamp(const QString &path) {
  xattr::Result raw = xattr::get(path, kEmbeddedXattr);
  if (!raw.value) return std::nullopt;
  if (raw.value->startsWith("r237:")) {
    const auto keys = vault::keysFor(path);
    if (!keys) return std::nullopt;
    raw.value = vault::openAttribute(*keys, path, *raw.value);
    if (!raw.value) return std::nullopt;
  }
  bool ok = false;
#ifdef Q_OS_WIN
  const qint64 value = QString::fromUtf8(*raw.value).trimmed().toLongLong(&ok);
#else
  const QString text = QString::fromUtf8(*raw.value);
  if (text.toUtf8() != *raw.value) return std::nullopt;
  const qint64 value = text.toLongLong(&ok);
#endif
  if (!ok || !supportedTimestamp(value)) return std::nullopt;
  return value;
}

void writeEmbeddedTimestamp(const QString &path, qint64 timestamp) { xattr::set(path, kEmbeddedXattr, timestampValue(path, timestamp)); }

static std::optional<QString> embeddedTimestampStampIfCurrent(const QString &path, std::optional<qint64> expected) {
  const auto before = embeddedTimestampRevision(path);
  const auto actual = readEmbeddedTimestamp(path);
  const auto after = embeddedTimestampRevision(path);
  if (before == after && actual == expected) return after;
  return std::nullopt;
}

// --- Leases ------------------------------------------------------------------------------------------------------

std::unique_ptr<FileLock> lockAlbumMetadata(const QString &dir) {
  const QString real = canonical(dir);
  const QString identity = directoryId(real);
  // Outside the data directory so clearing derived/user metadata cannot unlink a lock held by a writer.
  auto lease = FileLock::acquire(path::join(real, QStringLiteral(".room237-metadata.lock")), kLeaseTimeout, std::chrono::milliseconds(5));
  if (directoryId(real) != identity) fail(QStringLiteral("Album changed while waiting for metadata lease"));
  return lease;
}

static std::pair<QStringList, std::vector<std::unique_ptr<FileLock>>> acquireMetadataLeases(const QStringList &dirs) {
  QStringList directories;
  for (const QString &dir : dirs) directories << canonical(dir);
  std::sort(directories.begin(), directories.end());
  directories.erase(std::unique(directories.begin(), directories.end()), directories.end());
  // Never wait for an external process while holding the global store mutex. Multi-album operations use the same
  // canonical order in every process.
  std::vector<std::unique_ptr<FileLock>> leases;
  for (const QString &dir : directories) leases.push_back(lockAlbumMetadata(dir));
  return {directories, std::move(leases)};
}

MetadataWriteGuard lockMetadataForWrite(const QStringList &dirs) {
  // A caller may hold a leaf lease from before a root marker existed. Do not wait behind the tree writer.
  auto tree = album_tree::tryRead(dirs);
  if (!tree) fail(QStringLiteral("Album tree is being renamed; retry metadata write"));
  auto [directories, leases] = acquireMetadataLeases(dirs);
  tree->ensureCurrent(dirs);
  MetadataWriteGuard guard;
  guard.leases = std::move(leases);
  guard.tree = std::move(tree);
  guard.directories = directories;
  return guard;
}

std::unique_ptr<MetadataWriteGuard> tryLockMetadataForWrite(const QString &dir) {
  const QString real = canonical(dir);
  auto tree = album_tree::tryRead({real});
  if (!tree) return nullptr;
  const QString identity = directoryId(real);
  auto lease = FileLock::tryAcquire(path::join(real, QStringLiteral(".room237-metadata.lock")));
  if (!lease) return nullptr;
  if (directoryId(real) != identity) fail(QStringLiteral("Album changed during metadata admission"));
  tree->ensureCurrent({real});
  auto guard = std::make_unique<MetadataWriteGuard>();
  guard->leases.push_back(std::move(lease));
  guard->tree = std::move(tree);
  guard->directories = {real};
  return guard;
}

std::pair<std::unique_lock<std::mutex>, MetadataWriteGuard> lockMetadataForMoveWithTree(const QStringList &dirs, const album_tree::TreeLease &tree) {
  auto [directories, leases] = acquireMetadataLeases(dirs);
  tree.ensureCurrent(dirs);
  std::unique_lock moving(mediaMoveLock);
  MetadataWriteGuard guard;
  guard.leases = std::move(leases);
  guard.directories = directories;
  return {std::move(moving), std::move(guard)};
}

std::pair<std::unique_lock<std::mutex>, MetadataWriteGuard> lockMetadataForMove(const QStringList &dirs) {
  auto tree = album_tree::read(dirs);
  auto result = lockMetadataForMoveWithTree(dirs, *tree);
  result.second.tree = std::move(tree);
  return result;
}

// --- Database ----------------------------------------------------------------------------------------------------

static void ensureMetaDir(const QString &dir) {
  // Metadata operations may race an album deletion. Only create the service directory, never resurrect a missing
  // album or one of its ancestors.
  const QString path = metaDir(dir);
  try {
    createDir(path);
  } catch (const Error &) {
    if (!lstat(path).isDir || !isDir(path)) throw;
  }
}

void ensureDatabase(const QString &dir) {
  if (!exists(albumMetaPath(dir))) {
    // Parse before creating anything. Corrupt legacy metadata must survive.
    ensureMetaDir(dir);
    album_db::initializeLegacy(albumMetaPath(dir));
  }
}

void prepareDatabase(const QString &dir) {
  auto guard = lockMetadataForWrite({dir});
  ensureDatabase(dir);
  album_db::FileReader reader(albumMetaPath(dir));
}

std::optional<FileMetaEntry> readFileEntry(const QString &path) {
  const QString dir = dirOf(path);
  const QString name = nameOf(path);
  if (exists(albumMetaPath(dir))) return album_db::readFile(albumMetaPath(dir), name);
  if (exists(metaDir(dir))) {
    prepareDatabase(dir);
    return album_db::readFile(albumMetaPath(dir), name);
  }
  return std::nullopt;
}

AlbumMeta readSelectedMetadata(const QString &dir, const QStringList &names, bool related) {
  if (!exists(metaDir(dir))) return {};
  auto guard = lockMetadataForWrite({dir});
  ensureDatabase(dir);
  return album_db::subset(albumMetaPath(dir), names, related);
}

AlbumMeta readSelectedWithGuard(const QString &dir, const QStringList &names, bool related, const MetadataWriteGuard &guard) {
  if (!guard.covers(canonical(dir))) fail(QStringLiteral("Album metadata lease is required"));
  if (!exists(metaDir(dir))) return {};
  ensureDatabase(dir);
  return album_db::subset(albumMetaPath(dir), names, related);
}

static void writeSelected(const QString &dir, const QStringList &names, const AlbumMeta &album) {
  ensureDatabase(dir);
  album_db::patch(albumMetaPath(dir), names, album);
}

void removeDeletedMetadataWithGuard(const QString &dir, const QStringList &names, const MetadataWriteGuard &guard) {
  if (!guard.covers(canonical(dir))) fail(QStringLiteral("Album metadata lease is required"));
  ensureDatabase(dir);
  QStringList removed;
  for (const QString &name : names) {
    validateFileName(name);
    if (!entryExists(path::join(dir, name))) removed << name;
  }
  album_db::remove(albumMetaPath(dir), removed);
  try {
    catalog::forget(dir, removed);
  } catch (const Error &error) {
    qWarning("Catalog cleanup failed: %s", qUtf8Printable(error.message()));
  }
}

FileMetaEntry updateFileMeta(const QString &dir, const QString &name, const std::function<void(FileMetaEntry &)> &update) {
  validateFileName(name);
  auto guard = lockMetadataForWrite({dir});
  ensureDatabase(dir);
  return album_db::updateRows<FileMetaEntry>(albumMetaPath(dir), [&](db::Connection &conn, album_db::Rows &rows) {
    FileMetaEntry entry = album_db::entry(conn, name);
    update(entry);
    rows.insert(name, entry);
    return entry;
  });
}

// --- Batched derived writes --------------------------------------------------------------------------------------

namespace {
constexpr size_t kBatchSize = 32;
constexpr auto kBatchWait = std::chrono::milliseconds(8);
constexpr size_t kMaxOutstanding = 96;  // 64 queued + 32 in flight.

struct DerivedWrite {
  QString path, name;
  std::optional<QString> expectedRevision, expectedSource;
  std::function<void(FileMetaEntry &)> update;
  std::shared_ptr<std::promise<FileMetaEntry>> response;
  std::shared_ptr<Budget::Permit> permit;
};

void reply(DerivedWrite &write, const QString &error) {
  try {
    write.response->set_exception(std::make_exception_ptr(Error(error)));
  } catch (...) {
  }
}

void flushDerivedAlbum(const QString &dir, std::vector<DerivedWrite> writes) {
  try {
    ensureDatabase(dir);
  } catch (const Error &error) {
    for (auto &write : writes) reply(write, error.message());
    return;
  }
  struct Completed {
    std::shared_ptr<std::promise<FileMetaEntry>> response;
    std::optional<FileMetaEntry> entry;
    QString error;
    std::shared_ptr<Budget::Permit> permit;
  };
  std::vector<Completed> completed;
  bool consumed = false;
  QString persistError;
  try {
    album_db::updateRowsVoid(albumMetaPath(dir), [&](db::Connection &conn, album_db::Rows &rows) {
      consumed = true;
      for (auto &write : writes) {
        if (!isFile(write.path) || (write.expectedRevision && mediaRevision(write.path) != *write.expectedRevision) || (write.expectedSource && sourceRevision(write.path) != write.expectedSource)) {
          reply(write, QStringLiteral("Media changed during processing: %1").arg(write.path));
          continue;
        }
        Completed done{write.response, std::nullopt, {}, write.permit};
        try {
          if (!rows.contains(write.name)) rows.insert(write.name, album_db::entry(conn, write.name));
          FileMetaEntry &entry = rows[write.name];
          write.update(entry);
          done.entry = entry;
        } catch (const Error &error) {
          done.error = error.message();
        }
        completed.push_back(std::move(done));
      }
    });
  } catch (const Error &error) {
    persistError = error.message();
  }
  if (!consumed) {
    for (auto &write : writes) reply(write, persistError);
    return;
  }
  // A worker receives success only after the entire transaction commits.
  for (Completed &done : completed) {
    try {
      if (!persistError.isEmpty()) done.response->set_exception(std::make_exception_ptr(Error(persistError)));
      else if (!done.error.isEmpty()) done.response->set_exception(std::make_exception_ptr(Error(done.error)));
      else done.response->set_value(*done.entry);
    } catch (...) {
    }
  }
}

void flushDerivedWrites(std::vector<DerivedWrite> writes) {
  std::map<QString, std::vector<DerivedWrite>> albums;
  for (auto &write : writes) albums[path::parent(write.path)].push_back(std::move(write));
  for (auto &[dir, items] : albums) {
    std::optional<MetadataWriteGuard> guard;
    try {
      guard.emplace(lockMetadataForWrite({dir}));
    } catch (const Error &error) {
      for (auto &write : items) reply(write, error.message());
      continue;
    }
    flushDerivedAlbum(dir, std::move(items));
  }
}

class DerivedWriter {
public:
  explicit DerivedWriter(std::chrono::milliseconds timeout) : timeout_(timeout), budget_(int(kMaxOutstanding)) {
    std::thread([this] {
      setLowPriorityCurrentThread();
      run();
    }).detach();
  }
  void send(DerivedWrite write) {
    write.permit = std::make_shared<Budget::Permit>(budget_);
    std::unique_lock lock(mutex_);
    spaceAvailable_.wait(lock, [this] { return queue_.size() < 64; });
    queue_.push_back(std::move(write));
    itemAvailable_.notify_one();
  }

private:
  struct Pending {
    DerivedWrite write;
    std::optional<std::chrono::steady_clock::time_point> waitingSince;
  };
  bool receive(DerivedWrite &out, std::optional<std::chrono::steady_clock::time_point> deadline) {
    std::unique_lock lock(mutex_);
    if (deadline) {
      if (!itemAvailable_.wait_until(lock, *deadline, [this] { return !queue_.empty(); })) return false;
    } else {
      itemAvailable_.wait(lock, [this] { return !queue_.empty(); });
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    spaceAvailable_.notify_one();
    return true;
  }
  void run() {
    std::vector<Pending> pending;
    for (;;) {
      const bool hadPending = !pending.empty();
      if (!hadPending) {
        DerivedWrite write;
        receive(write, std::nullopt);
        pending.push_back({std::move(write), std::nullopt});
      }
      // Ready leftovers need no extra batching delay. Busy leases are polled at the normal batch interval while
      // newly arriving albums join the batch.
      bool anyReady = false;
      for (const Pending &p : pending) anyReady |= !p.waitingSince;
      const auto wait = hadPending && anyReady ? std::chrono::milliseconds(0) : kBatchWait;
      const auto deadline = std::chrono::steady_clock::now() + wait;
      size_t received = hadPending ? 0 : 1;
      while (pending.size() < kMaxOutstanding) {
        DerivedWrite write;
        if (!receive(write, deadline)) break;
        pending.push_back({std::move(write), std::nullopt});
        if (++received >= kBatchSize) break;
      }
      if (pending.size() == kMaxOutstanding && wait.count() > 0) std::this_thread::sleep_until(deadline);
      std::map<QString, std::vector<Pending>> albums;
      for (Pending &p : pending) albums[path::parent(p.write.path)].push_back(std::move(p));
      pending.clear();
      for (auto &[dir, writes] : albums) {
        std::unique_ptr<MetadataWriteGuard> guard;
        QString error;
        try {
          guard = tryLockMetadataForWrite(dir);
        } catch (const Error &e) {
          error = e.message();
        }
        if (!error.isEmpty()) {
          for (Pending &p : writes) reply(p.write, error);
        } else if (guard) {
          std::vector<DerivedWrite> batch;
          size_t i = 0;
          for (; i < writes.size() && i < kBatchSize; ++i) batch.push_back(std::move(writes[i].write));
          flushDerivedAlbum(dir, std::move(batch));
          for (; i < writes.size(); ++i) pending.push_back({std::move(writes[i].write), std::nullopt});
        } else {
          const auto now = std::chrono::steady_clock::now();
          for (Pending &p : writes) {
            const auto started = p.waitingSince.value_or(now);
            p.waitingSince = started;
            if (now - started >= timeout_) reply(p.write, QStringLiteral("File lock timed out: %1").arg(path::join(dir, QStringLiteral(".room237-metadata.lock"))));
            else pending.push_back(std::move(p));
          }
        }
      }
    }
  }
  std::chrono::milliseconds timeout_;
  Budget budget_;
  std::mutex mutex_;
  std::condition_variable itemAvailable_, spaceAvailable_;
  std::deque<DerivedWrite> queue_;
};

DerivedWriter &derivedWriter() {
  static DerivedWriter *writer = new DerivedWriter(kLeaseTimeout);
  return *writer;
}

FileMetaEntry submitDerivedWrite(const QString &path, const std::optional<QString> &expectedRevision, const std::optional<QString> &expectedSource, std::function<void(FileMetaEntry &)> update) {
  if (path::parent(path).isEmpty()) fail(QStringLiteral("Invalid path"));
  const QString name = nameOf(path);
  validateFileName(name);
  DerivedWrite write;
  write.path = path;
  write.name = name;
  write.expectedRevision = expectedRevision;
  write.expectedSource = expectedSource;
  write.update = std::move(update);
  write.response = std::make_shared<std::promise<FileMetaEntry>>();
  std::future<FileMetaEntry> result = write.response->get_future();
  if (backgroundWrites) derivedWriter().send(std::move(write));
  else {
    std::vector<DerivedWrite> writes;
    writes.push_back(std::move(write));
    flushDerivedWrites(std::move(writes));
  }
  return result.get();
}
}  // namespace

BackgroundMetadataWrites::BackgroundMetadataWrites() : previous_(backgroundWrites) { backgroundWrites = true; }
BackgroundMetadataWrites::~BackgroundMetadataWrites() { backgroundWrites = previous_; }

FileMetaEntry updateFileMetaIfSourceCurrent(const QString &path, const QString &expectedSource, std::function<void(FileMetaEntry &)> update) { return submitDerivedWrite(path, std::nullopt, expectedSource, std::move(update)); }
FileMetaEntry updateFileMetaIfCurrent(const QString &path, const QString &expectedRevision, const std::optional<QString> &expectedSource, std::function<void(FileMetaEntry &)> update) { return submitDerivedWrite(path, expectedRevision, expectedSource, std::move(update)); }

// --- Cache validity ----------------------------------------------------------------------------------------------

bool hashCacheCurrent(const QString &path, const FileMetaEntry &entry) {
  if (entry.hashFailed) {
    const AdvancedSettings settings = readSettings();
    return entry.hashFailedProcessingVersion == settings.processingVersion() && sourceRevision(path) == entry.hashFailedSourceRevision && entry.hashFailedSourceRevision.has_value() &&
           (!settings.preload.thumbHashRetryOnThumbChange || entry.hashSourceRevision == mediaRevision(path));
  }
  return isFile(path) && entry.hashSourceRevision == mediaRevision(path);
}

bool metadataContentCacheCurrent(const QString &path, const FileMetaEntry &entry, const AdvancedSettings &settings) {
  return entry.metaSettingsVersion == settings.metadataVersion() && entry.metaSourceRevision.has_value() && entry.metaSourceRevision == sourceRevision(path) && entry.meta && validPackedMeta(*entry.meta);
}
bool metadataCacheCurrentWithSettings(const QString &path, const FileMetaEntry &entry, const AdvancedSettings &settings) {
  if (!metadataContentCacheCurrent(path, entry, settings)) return false;
  if (entry.metaEmbeddedRevision && embeddedTimestampRevision(path) == entry.metaEmbeddedRevision) return true;
  return entry.metaEmbeddedTimestamp == readEmbeddedTimestamp(path);
}
bool metadataCacheCurrent(const QString &path, const FileMetaEntry &entry) { return metadataCacheCurrentWithSettings(path, entry, readSettings()); }

static QString thumbFailureProcessingVersion(const QString &path, const QString &processingVersion) {
  if (const auto name = thumb::detachedThumbnailName(path)) return processingVersion + QStringLiteral("|preview-name:") + *name;
  return processingVersion;
}

void markThumbFailed(const QString &path, const QString &expectedSource, const QString &processingVersion) {
  const QString version = thumbFailureProcessingVersion(path, processingVersion);
  updateFileMetaIfSourceCurrent(path, expectedSource, [=](FileMetaEntry &entry) {
    entry.thumbFailedProcessingVersion = version;
    entry.thumbFailed = true;
    entry.thumbFailedSourceRevision = expectedSource;
  });
}
void markMetaFailed(const QString &path, const QString &expectedSource, const QString &processingVersion) {
  updateFileMetaIfSourceCurrent(path, expectedSource, [=](FileMetaEntry &entry) {
    entry.metaFailedProcessingVersion = processingVersion;
    entry.metaFailed = true;
    entry.metaFailedSourceRevision = expectedSource;
  });
}

bool isThumbFailed(const QString &path) {
  std::optional<FileMetaEntry> entry;
  try {
    entry = readFileEntry(path);
  } catch (const Error &) {
    return false;
  }
  return entry && entry->thumbFailed && entry->thumbFailedProcessingVersion == thumbFailureProcessingVersion(path, readSettings().processingVersion()) && entry->thumbFailedSourceRevision.has_value() &&
         entry->thumbFailedSourceRevision == sourceRevision(path);
}
bool isMetaFailed(const QString &path) {
  std::optional<FileMetaEntry> entry;
  try {
    entry = readFileEntry(path);
  } catch (const Error &) {
    return false;
  }
  return entry && entry->metaFailed && entry->metaFailedProcessingVersion == readSettings().processingVersion() && entry->metaFailedSourceRevision.has_value() && entry->metaFailedSourceRevision == sourceRevision(path);
}

void markHashFailedIfCurrent(const QString &path, const QString &expectedRevision, const HashConfig &config, const QString &processingVersion) {
  const quint32 bits = config.bits;
  const QString version = config.hashVersion;
  updateFileMetaIfCurrent(path, expectedRevision, std::nullopt, [=](FileMetaEntry &entry) {
    entry.hashFailedProcessingVersion = processingVersion;
    entry.hash.reset();
    entry.hashBits = bits;
    entry.hashVersion = version;
    entry.hashSourceRevision = expectedRevision;
    entry.hashFailedSourceRevision = sourceRevision(path);
    entry.hashFailed = true;
  });
}

std::optional<CachedHash> loadAlbumFileHash(const QString &path) {
  std::optional<FileMetaEntry> entry;
  try {
    entry = readFileEntry(path);
  } catch (const Error &) {
    return std::nullopt;
  }
  if (!entry || !hashCacheCurrent(path, *entry) || !entry->hash) return std::nullopt;
  return CachedHash{*entry->hash, entry->hashVersion, entry->hashBits};
}

void writeAlbumFileHash(const QString &path, const QString &hashB64, const QString &hashVersion, quint32 hashBits, const QString &expectedRevision) {
  updateFileMetaIfCurrent(path, expectedRevision, std::nullopt, [=](FileMetaEntry &entry) {
    entry.hash = hashB64;
    entry.hashVersion = hashVersion;
    entry.hashBits = hashBits;
    entry.hashSourceRevision = expectedRevision;
    entry.hashFailedSourceRevision.reset();
    entry.hashFailed = false;
  });
}

void writeThumbState(const QString &path, const QString &thumbVersion, const QString &expectedSource, std::optional<QString> sourceHash, std::optional<QString> hashRevision, std::optional<QString> blurPreview) {
  if (sourceRevision(path) != expectedSource) fail(QStringLiteral("Media changed during thumbnail generation"));
  updateFileMetaIfCurrent(path, mediaRevision(path), expectedSource, [=](FileMetaEntry &entry) {
    entry.blurPreview = blurPreview;
    entry.thumbSourceRevision = expectedSource;
    entry.thumbSourceHash = sourceHash;
    entry.thumbHashRevision = hashRevision;
    entry.thumbVersion = thumbVersion;
    entry.thumbFailed = false;
    entry.thumbFailedSourceRevision.reset();
  });
}

// --- Extraction --------------------------------------------------------------------------------------------------

StoredMetadata getMetadataWithFavoriteWithSettings(const QString &path, const AdvancedSettings &settings) {
  if (auto entry = readFileEntry(path); entry && metadataCacheCurrentWithSettings(path, *entry, settings)) return {*entry->meta, entry->favorite};
  getFileMetadataWithSettings(path, settings);
  auto entry = readFileEntry(path);
  if (!entry || !metadataCacheCurrentWithSettings(path, *entry, settings)) fail(QStringLiteral("Media changed during metadata extraction"));
  return {*entry->meta, entry->favorite};
}
StoredMetadata getMetadataWithFavorite(const QString &path) { return getMetadataWithFavoriteWithSettings(path, readSettings()); }

static std::optional<qint64> parseRfc3339(const QString &text) {
  static const QRegularExpression pattern(QStringLiteral("^(\\d{4})-(\\d{2})-(\\d{2})[Tt ](\\d{2}):(\\d{2}):(\\d{2})(\\.\\d+)?([Zz]|[+-]\\d{2}:\\d{2})$"));
  const auto match = pattern.match(text);
  if (!match.hasMatch()) return std::nullopt;
  const QDate date(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
  const int hour = match.captured(4).toInt(), minute = match.captured(5).toInt(), second = match.captured(6).toInt();
  if (!date.isValid() || hour > 23 || minute > 59 || second > 60) return std::nullopt;
  qint64 seconds = QDateTime(date, QTime(hour, minute, qMin(second, 59)), QTimeZone::UTC).toSecsSinceEpoch() + (second == 60 ? 1 : 0);
  const QString zone = match.captured(8);
  if (zone.size() == 6) {
    const int offset = zone.mid(1, 2).toInt() * 3600 + zone.mid(4, 2).toInt() * 60;
    if (zone.mid(1, 2).toInt() > 23 || zone.mid(4, 2).toInt() > 59) return std::nullopt;
    seconds -= zone[0] == QLatin1Char('-') ? -offset : offset;
  }
  return seconds;
}

struct ProbeResult {
  std::optional<qint64> shoot;
  std::optional<quint32> width, height;
};

static ProbeResult parseProbeOutput(const QString &stderrText, bool parseCreationTime) {
  ProbeResult result;
  for (const QString &line : stderrText.split(QLatin1Char('\n'))) {
    QString l = line;
    if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
    if (!result.width && l.contains(QLatin1String("Stream")) && l.contains(QLatin1String("Video:"))) {
      const QStringList tokens = l.split(QLatin1Char(','));
      for (int i = 1; i < tokens.size(); ++i) {
        const QString token = tokens[i].trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).value(0);
        const int x = token.indexOf(QLatin1Char('x'));
        if (x < 0) continue;
        bool okW = false, okH = false;
        const uint w = token.left(x).toUInt(&okW), h = token.mid(x + 1).toUInt(&okH);
        if (okW && okH && w > 0 && h > 0 && !token.left(x).startsWith(QLatin1Char('+')) && !token.mid(x + 1).startsWith(QLatin1Char('+'))) {
          result.width = w;
          result.height = h;
          break;
        }
      }
    }
    if (parseCreationTime) {
      const int at = l.indexOf(QLatin1String("creation_time"));
      if (at >= 0) {
        QString time = l.mid(at + 13).trimmed();
        while (time.startsWith(QLatin1Char(':'))) time.remove(0, 1);
        if (const auto parsed = parseRfc3339(time.trimmed())) result.shoot = supportedTimestamp(*parsed) ? std::optional(*parsed) : std::nullopt;
      }
    }
  }
  return result;
}

static ProbeResult probeWithSettings(const QString &path, bool isVideo, const AdvancedSettings &settings) {
  // Dimensions of a still image are in its header; only containers need a decoder probe.
  if (!isVideo && native_image::metadataEnabled())
    if (const auto size = native_image::dimensions(path)) return {std::nullopt, size->first, size->second};
  DecodePermit permit;
  QStringList args = ffmpegTuning(isVideo);
  // One frame is enough for dimensions and stream tags; do not decode an entire video.
  args << QStringLiteral("-hide_banner") << QStringLiteral("-i") << path << QStringLiteral("-frames:v") << QStringLiteral("1") << QStringLiteral("-an") << QStringLiteral("-f") << QStringLiteral("null") << QStringLiteral("-");
  const quint64 timeout = settings.metadata.ffmpegProbeTimeoutSecs.value_or(settings.ffmpeg.timeoutSecs);
  const ProcessResult output = runChecked(ffmpegPath(), args, int(timeout * 1000));
  const bool turned = !isVideo && probe::exifTurnsPicture(path);
  ProbeResult result = parseProbeOutput(QString::fromUtf8(output.stderrBytes), settings.metadata.parseCreationTime);
  if (turned) std::swap(result.width, result.height);
  return result;
}

static ProbeResult probeEncrypted(QIODevice &plain, const QString &ext, bool isVideo, const AdvancedSettings &settings) {
  if (!isVideo) {
    plain.seek(0);
    const QByteArray bytes = plain.read(qint64(1) << 30);
    if (const auto size = native_image::dimensionsFromData(bytes)) return {std::nullopt, size->first, size->second};
  }
  DecodePermit permit;
  plain.seek(0);
  QStringList args = ffmpegTuning(isVideo);
  args << QStringLiteral("-hide_banner") << QStringLiteral("-i") << QStringLiteral("pipe:0") << QStringLiteral("-frames:v") << QStringLiteral("1") << QStringLiteral("-an") << QStringLiteral("-f") << QStringLiteral("null") << QStringLiteral("-");
  const quint64 timeout = settings.metadata.ffmpegProbeTimeoutSecs.value_or(settings.ffmpeg.timeoutSecs);
  const ProcessResult output = runWithInput(ffmpegPath(), args, plain, int(timeout * 1000) * 4);
  if (!output.success && output.stderrBytes.indexOf("Video:") < 0) fail(QStringLiteral("Cannot read metadata: %1").arg(QString::fromUtf8(output.stderrBytes)));
  ProbeResult result = parseProbeOutput(QString::fromUtf8(output.stderrBytes), settings.metadata.parseCreationTime);
  if (!isVideo && (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") || ext == QLatin1String("tif") || ext == QLatin1String("tiff"))) {
    plain.seek(0);
    if (const auto exif = probe::readExif(plain); exif && exif->orientation && *exif->orientation >= 5) std::swap(result.width, result.height);
  }
  return result;
}

QString getFileMetadataWithSettings(const QString &path, const AdvancedSettings &settings) {
  const auto source = sourceRevision(path);
  if (!source) fail(QStringLiteral("Media file does not exist"));
  const FileInfo info = stat(path);
  std::optional<quint64> added;
  if (info.birthSec && *info.birthSec >= 0) added = quint64(*info.birthSec);
  QString ext = path::extension(path).toLower();
  bool inVault = false;
  const auto keys = vault::keysFor(path, &inVault);
  std::optional<vault::FileMeta> sealed;
  if (keys && vault::isEncryptedFile(path)) {
    // The original's name and creation time live inside the encrypted file.
    sealed = vault::readMeta(*keys, path);
    ext = path::extension(sealed->name).toLower();
    added = sealed->birthSec && *sealed->birthSec >= 0 ? std::optional<quint64>(quint64(*sealed->birthSec)) : added;
  } else if (inVault && !keys) {
    fail(QStringLiteral("This library is locked"));
  }
  const bool isImage = constants::isImageExtension(ext);
  const bool isVideo = constants::isVideoExtension(ext);
  ProbeResult probed;
  std::unique_ptr<QIODevice> plain;
  if (sealed) plain = vault::openPlain(path);
  if ((isImage || isVideo) && plain) probed = probeEncrypted(*plain, ext, isVideo, settings);
  else if (isImage || isVideo) probed = probeWithSettings(path, isVideo, settings);
  // FFmpeg treats QuickTime dates before 1970 as Unix timestamps, shifting standards-compliant dates by 66 years.
  // Correct only that exact shift; distinct explicit/stream tags retain their existing precedence.
  std::optional<qint64> containerShoot;
  if (settings.metadata.parseCreationTime && (ext == QLatin1String("mov") || ext == QLatin1String("mp4") || ext == QLatin1String("m4v"))) {
    const auto quick = plain ? (plain->seek(0), probe::quickTimeCreationTime(*plain, quint64(plain->size()))) : probe::quickTimeCreationTime(path);
    if (quick && *quick < 0 && supportedTimestamp(*quick) && probed.shoot && *quick + probe::kQuickTimeEpochOffset == *probed.shoot) containerShoot = quick;
  }
  std::optional<qint64> fallback = containerShoot ? containerShoot : probed.shoot;
  if (!fallback && isImage) {
    if (plain) {
      if (const auto exif = probe::readExif(*plain)) {
        const auto text = exif->dateTimeOriginal ? exif->dateTimeOriginal : exif->dateTime;
        const QDateTime parsed = text ? QDateTime::fromString(*text, QStringLiteral("yyyy:MM:dd HH:mm:ss")) : QDateTime();
        if (parsed.isValid()) fallback = QDateTime(parsed.date(), parsed.time(), QTimeZone::UTC).toSecsSinceEpoch();
      }
    } else {
      fallback = probe::datetimeOriginal(path);
    }
  }
  DetachedFileMeta meta{added, fallback, isImage, isVideo, probed.width, probed.height};
  const QString version = settings.metadataVersion();
  const QString sourceValue = *source;
  const FileMetaEntry written = updateFileMetaIfSourceCurrent(path, sourceValue, [=](FileMetaEntry &entry) mutable {
    // A user may have changed the date while FFmpeg was probing this file.
    const auto before = embeddedTimestampRevision(path);
    const auto embedded = readEmbeddedTimestamp(path);
    const auto after = embeddedTimestampRevision(path);
    // Read the manual override at publication, including its removal.
    meta.s = embedded ? embedded : fallback;
    entry.metaEmbeddedTimestamp = embedded;
    entry.metaEmbeddedRevision = before == after ? after : std::nullopt;
    entry.meta = meta.pack();
    entry.metaSourceRevision = sourceValue;
    entry.metaSettingsVersion = version;
    entry.metaFailed = false;
    entry.metaFailedSourceRevision.reset();
  });
  if (!written.meta) fail(QStringLiteral("Metadata result was not persisted"));
  return *written.meta;
}
QString getFileMetadata(const QString &path) { return getFileMetadataWithSettings(path, readSettings()); }
QString getFileMetadataCached(const QString &path) { return getMetadataWithFavorite(path).meta; }
QString getFileMetadataCachedWithSettings(const QString &path, const AdvancedSettings &settings) { return getMetadataWithFavoriteWithSettings(path, settings).meta; }

void invalidateMediaContent(const QString &path) {
  updateFileMeta(dirOf(path), nameOf(path), [](FileMetaEntry &entry) {
    entry.meta.reset();
    entry.metaSourceRevision.reset();
    entry.metaEmbeddedTimestamp.reset();
    entry.metaEmbeddedRevision.reset();
    entry.metaSettingsVersion.reset();
    entry.metaFailedSourceRevision.reset();
    entry.thumbFailedSourceRevision.reset();
    entry.hash.reset();
    entry.hashVersion.reset();
    entry.hashBits.reset();
    entry.hashSourceRevision.reset();
    entry.hashFailedSourceRevision.reset();
    entry.thumbHashRevision.reset();
    entry.hashFailed = entry.thumbFailed = entry.metaFailed = false;
  });
}

// --- HEIC companions ---------------------------------------------------------------------------------------------

bool isImageMedia(const QString &album, const QString &name) {
  if (!constants::isEncryptedName(name)) return constants::isImageExtension(constants::lowerExtension(name));
  try {
    const auto entry = readFileEntry(path::join(album, name));
    if (entry && entry->vaultName) return constants::isImageExtension(constants::lowerExtension(*entry->vaultName));
    if (entry && entry->meta && validPackedMeta(*entry->meta)) return unpackFileMeta(*entry->meta).i;
  } catch (const Error &) {
  }
  return false;
}

bool heicHiddenByEntry(const QString &album, const FileMetaEntry &entry) {
  return entry.heicConversion && validFileName(entry.heicConversion->outputName) && lstat(path::join(album, entry.heicConversion->outputName)).isFile;
}

QSet<QString> hiddenHeicNames(const QString &album) {
  const QString database = path::join(album, QStringLiteral(".room237-metadata/album.sqlite"));
  if (!isFile(database)) return {};
  album_db::FileReader reader(database);
  QSet<QString> hidden;
  for (const DirEntry &file : readDir(album)) {
    if (!path::hasExtension(file.name, {"heic"})) continue;
    if (auto entry = reader.get(file.name); entry && heicHiddenByEntry(album, *entry)) hidden.insert(file.name);
  }
  return hidden;
}

bool heicBindingCurrent(const QString &dir, const QString &name, const HeicConversion &binding) {
  validateFileName(name);
  validateFileName(binding.outputName);
  const QString raw = path::join(dir, name), jpeg = path::join(dir, binding.outputName);
  return path::hasExtension(raw, {"heic"}) && path::hasExtension(jpeg, {"jpg", "jpeg"}) && lstat(raw).isFile && lstat(jpeg).isFile && sourceRevision(raw) == binding.sourceRevision && sourceRevision(jpeg) == binding.outputRevision;
}

// --- Move journals -----------------------------------------------------------------------------------------------

void MoveMetadataUndo::validate() const {
  for (const auto *rows : {&source, &target}) {
    QSet<QString> seen;
    for (const MoveMetadataRow &row : *rows) {
      validateFileName(row.name);
      if (seen.contains(row.name)) fail(QStringLiteral("Duplicate move metadata row"));
      seen.insert(row.name);
      for (const auto *entry : {&row.before, &row.after})
        if (*entry && (*entry)->heicConversion) validateFileName((*entry)->heicConversion->outputName);
    }
  }
}

void MoveMetadataUndo::validateCurrent(const AlbumMeta &sourceAlbum, const AlbumMeta &targetAlbum) const {
  validate();
  for (const auto &[album, rows] : {std::pair{&sourceAlbum, &source}, std::pair{&targetAlbum, &target}}) {
    for (const MoveMetadataRow &row : *rows) {
      auto it = album->files.constFind(row.name);
      const std::optional<FileMetaEntry> current = it == album->files.cend() ? std::nullopt : std::optional(*it);
      if (current != row.before && current != row.after) fail(QStringLiteral("Move metadata changed; preserving %1").arg(row.name));
    }
  }
}

static QString rowsJson(const std::vector<MoveMetadataRow> &rows) {
  QStringList items;
  for (const MoveMetadataRow &row : rows)
    items << json::Object().str("name", row.name).raw("before", row.before ? row.before->toJson() : QStringLiteral("null")).raw("after", row.after ? row.after->toJson() : QStringLiteral("null")).compact();
  return QLatin1Char('[') + items.join(QLatin1Char(',')) + QLatin1Char(']');
}
QString MoveMetadataUndo::toJson() const { return json::Object().raw("source", rowsJson(source)).raw("target", rowsJson(target)).compact(); }

MoveMetadataUndo MoveMetadataUndo::fromJson(const QJsonObject &object) {
  auto rows = [](const QJsonValue &value, const char *field) {
    if (!value.isArray()) fail(QStringLiteral("missing field `%1`").arg(QLatin1String(field)));
    std::vector<MoveMetadataRow> out;
    for (const QJsonValue &item : value.toArray()) {
      const QJsonObject o = item.toObject();
      MoveMetadataRow row;
      row.name = json::stringField(o.value(QLatin1String("name")), "name");
      const QJsonValue before = o.value(QLatin1String("before")), after = o.value(QLatin1String("after"));
      if (before.isObject()) row.before = FileMetaEntry::fromJsonObject(before.toObject());
      if (after.isObject()) row.after = FileMetaEntry::fromJsonObject(after.toObject());
      out.push_back(row);
    }
    return out;
  };
  MoveMetadataUndo undo;
  undo.source = rows(object.value(QLatin1String("source")), "source");
  undo.target = rows(object.value(QLatin1String("target")), "target");
  return undo;
}

static QStringList rowNames(const std::vector<MoveMetadataRow> &rows) {
  QStringList names;
  for (const MoveMetadataRow &row : rows) names << row.name;
  return names;
}

void validateMoveMetadataUndoWithGuard(const QString &source, const QString &target, const MoveMetadataUndo &undo, const MetadataWriteGuard &guard) {
  const AlbumMeta sourceAlbum = readSelectedWithGuard(source, rowNames(undo.source), false, guard);
  const AlbumMeta targetAlbum = readSelectedWithGuard(target, rowNames(undo.target), false, guard);
  undo.validateCurrent(sourceAlbum, targetAlbum);
}

void restoreMoveMetadataWithHooks(const QString &source, const QString &target, const MoveMetadataUndo &undo, const MetadataWriteGuard &guard, const std::function<void(const QString &)> &checkpoint) {
  undo.validate();
  AlbumMeta sourceAlbum = readSelectedWithGuard(source, rowNames(undo.source), false, guard);
  AlbumMeta targetAlbum = readSelectedWithGuard(target, rowNames(undo.target), false, guard);
  // Check both albums before publishing either rollback. Rows unrelated to this move, and writers that changed a
  // touched row, remain untouched.
  undo.validateCurrent(sourceAlbum, targetAlbum);
  const std::tuple<const QString *, AlbumMeta *, const std::vector<MoveMetadataRow> *> steps[] = {{&target, &targetAlbum, &undo.target}, {&source, &sourceAlbum, &undo.source}};
  for (const auto &[dir, album, rows] : steps) {
    bool changed = false;
    for (const MoveMetadataRow &row : *rows) {
      auto it = album->files.constFind(row.name);
      const std::optional<FileMetaEntry> current = it == album->files.cend() ? std::nullopt : std::optional(*it);
      if (current != row.before) {
        if (row.before) album->files.insert(row.name, *row.before);
        else album->files.remove(row.name);
        changed = true;
      }
    }
    if (changed) {
      writeSelected(*dir, rowNames(*rows), *album);
      checkpoint(dir == &target ? QStringLiteral("move_recovery_metadata_target") : QStringLiteral("move_recovery_metadata_source"));
    }
  }
}

void transferMediaState(const QString &sourceDir, const QString &targetDir, const std::vector<std::pair<QString, QString>> &moves, const MetadataWriteGuard &guard, bool transferFields,
                        const std::function<void(const MoveMetadataUndo &)> &prepare, const std::function<void(const QString &)> &checkpoint) {
  if (moves.empty() || sourceDir == targetDir) return;
  for (const QString &dir : {sourceDir, targetDir})
    if (!guard.covers(canonical(dir))) fail(QStringLiteral("Metadata transfer requires leases for both albums"));
  if (transferFields || exists(metaDir(sourceDir))) ensureDatabase(sourceDir);
  ensureDatabase(targetDir);
  QStringList sourceNames, targetNames;
  for (const auto &[from, to] : moves) {
    sourceNames << from;
    targetNames << to;
  }
  AlbumMeta sourceAlbum = exists(albumMetaPath(sourceDir)) ? album_db::subset(albumMetaPath(sourceDir), sourceNames, false) : AlbumMeta();
  QHash<QString, FileMetaEntry> previousSource;
  for (const auto &[from, to] : moves)
    if (sourceAlbum.files.contains(from)) previousSource.insert(from, sourceAlbum.files.value(from));
  const AlbumMeta previousTarget = album_db::subset(albumMetaPath(targetDir), targetNames, false);
  AlbumMeta targetAlbum = previousTarget;
  bool bindingsChanged = false;
  QHash<QString, QString> names;
  for (const auto &[from, to] : moves) names.insert(from, to);
  for (const auto &[sourceName, targetName] : moves) {
    targetAlbum.files.remove(targetName);
    std::optional<FileMetaEntry> entry;
    if (transferFields) {
      if (sourceAlbum.files.contains(sourceName)) entry = sourceAlbum.files.take(sourceName);
    } else if (sourceAlbum.files.contains(sourceName)) {
      FileMetaEntry &current = sourceAlbum.files[sourceName];
      if (current.heicConversion) {
        FileMetaEntry moved;
        moved.heicConversion = current.heicConversion;
        current.heicConversion.reset();
        bindingsChanged = true;
        entry = moved;
      }
    }
    if (entry) {
      if (transferFields) {
        const QString preview = thumb::thumbPath(path::join(targetDir, targetName), path::join(targetDir, thumb::kThumbDir));
        // Artifact publication preserves an existing destination. Its bytes cannot inherit the moved source's
        // freshness marker.
        if (entryExists(preview)) {
          entry->thumbVersion.reset();
          entry->thumbSourceRevision.reset();
        }
      }
      if (entry->heicConversion) {
        const HeicConversion binding = *entry->heicConversion;
        entry->heicConversion.reset();
        if (names.contains(binding.outputName)) {
          const QString outputName = names.value(binding.outputName);
          const QString raw = path::join(targetDir, targetName), jpeg = path::join(targetDir, outputName);
          if (sourceRevision(raw) == binding.sourceRevision && sourceRevision(jpeg) == binding.outputRevision) entry->heicConversion = HeicConversion{outputName, binding.sourceRevision, binding.outputRevision};
        }
      }
      targetAlbum.files.insert(targetName, *entry);
    }
  }
  auto changes = [&](const QHash<QString, FileMetaEntry> &before, const AlbumMeta &after, bool source) {
    std::vector<MoveMetadataRow> rows;
    QSet<QString> seen;
    for (const auto &[from, to] : moves) {
      const QString &name = source ? from : to;
      if (seen.contains(name)) continue;
      seen.insert(name);
      const std::optional<FileMetaEntry> a = before.contains(name) ? std::optional(before.value(name)) : std::nullopt;
      const std::optional<FileMetaEntry> b = after.files.contains(name) ? std::optional(after.files.value(name)) : std::nullopt;
      if (a == b) continue;
      rows.push_back({name, a, b});
    }
    return rows;
  };
  prepare(MoveMetadataUndo{changes(previousSource, sourceAlbum, true), changes(previousTarget.files, targetAlbum, false)});
  // Never discard source metadata until the destination is persisted.
  writeSelected(targetDir, targetNames, targetAlbum);
  checkpoint(QStringLiteral("after_target_metadata"));
  if (!transferFields && !bindingsChanged) return;
  try {
    writeSelected(sourceDir, sourceNames, sourceAlbum);
  } catch (const Error &error) {
    try {
      writeSelected(targetDir, targetNames, previousTarget);
    } catch (const Error &rollback) {
      fail(QStringLiteral("%1; destination metadata rollback: %2").arg(error.message(), rollback.message()));
    }
    throw;
  }
  checkpoint(QStringLiteral("after_source_metadata"));
}

// --- Dates and favorites -----------------------------------------------------------------------------------------

#ifndef Q_OS_WIN
namespace {
int timestampBatchSize() {
  // Keep descriptors bounded, reserving room for the rest of the application.
  struct rlimit limit {};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return 32;
  if (limit.rlim_cur >= 1024) return 128;
  return int(std::clamp<rlim_t>(limit.rlim_cur / 2, 1, 32));
}

struct TimestampTarget {
  QString path;
  QByteArray written;
  xattr::OpenFile::Namespace ns;
  std::unique_ptr<xattr::OpenFile> file;
  static std::unique_ptr<TimestampTarget> open(const QString &path, const xattr::OpenFile::Namespace &ns) {
    auto target = std::make_unique<TimestampTarget>();
    target->path = path;
    target->ns = ns;
    target->file = std::make_unique<xattr::OpenFile>(path);
    if (!(target->file->identity() == ns) || xattr::OpenFile::pathIdentity(path) != ns) fail(QStringLiteral("Media changed while updating its date: %1").arg(path));
    return target;
  }
  bool current(const QString &source) const { return xattr::OpenFile::pathIdentity(path) == ns && sourceRevision(path) == source; }
  xattr::Result raw() const { return file->get(kEmbeddedXattr); }
  void writeRaw(const std::optional<QByteArray> &value) const {
    if (!value) {
      const xattr::Result existing = raw();
      if (existing.error) fail(QStringLiteral("os error %1").arg(existing.error));
      if (!existing.value) return;
      file->remove(kEmbeddedXattr);
      return;
    }
    file->set(kEmbeddedXattr, *value);
  }
  void restore(const std::optional<QByteArray> &previous, const QByteArray &written) const {
    // Address the opened object even if its original path was replaced.
    const xattr::Result now = raw();
    if (now.error) fail(QStringLiteral("os error %1").arg(now.error));
    if (now.value == previous) return;
    if (now.value != written) fail(QStringLiteral("Cannot restore date: another writer changed %1").arg(path));
    writeRaw(previous);
  }
};

struct PreparedTimestamp {
  QString name, path, source;
  xattr::OpenFile::Namespace ns;
  QString meta;
};

std::pair<std::vector<PreparedTimestamp>, std::optional<QString>> prepareTimestampUpdates(const QString &dir, const QStringList &names) {
  std::vector<PreparedTimestamp> prepared;
  const AdvancedSettings settings = readSettings();
  AlbumMeta album;
  try {
    album = readSelectedMetadata(dir, names, false);
  } catch (const Error &error) {
    return {prepared, error.message()};
  }
  for (const QString &name : names) {
    const QString path = path::join(dir, name);
    try {
      const auto source = sourceRevision(path);
      if (!source) fail(QStringLiteral("%1 does not exist").arg(path));
      const auto ns = xattr::OpenFile::pathIdentity(path);
      if (!ns) fail(QStringLiteral("Cannot stat media namespace"));
      // The requested date replaces any manual override. Reuse current content fields even if its xattr changed or
      // is unreadable; probing is necessary only for new content.
      QString meta;
      auto it = album.files.constFind(name);
      if (it != album.files.cend() && metadataContentCacheCurrent(path, *it, settings)) meta = *it->meta;
      else meta = getFileMetadataCachedWithSettings(path, settings);
      unpackFileMeta(meta);
      prepared.push_back({name, path, *source, *ns, meta});
    } catch (const Error &error) {
      return {prepared, error.message()};
    }
  }
  return {prepared, std::nullopt};
}

DetachedMediaEntry applyTimestampMetadata(AlbumMeta &album, const PreparedTimestamp &record, qint64 timestamp, const QString &version) {
  FileMetaEntry &entry = album.files[record.name];
  DetachedFileMeta meta;
  if (entry.meta && entry.metaSourceRevision == record.source && validPackedMeta(*entry.meta)) meta = unpackFileMeta(*entry.meta);
  else meta = unpackFileMeta(record.meta);
  meta.s = timestamp;
  const QString packed = meta.pack();
  entry.meta = packed;
  entry.metaSourceRevision = record.source;
  entry.metaEmbeddedTimestamp = timestamp;
  entry.metaEmbeddedRevision = embeddedTimestampStampIfCurrent(record.path, timestamp);
  entry.metaSettingsVersion = version;
  entry.metaFailed = false;
  entry.metaFailedSourceRevision.reset();
  DetachedMediaEntry out;
  out.blurPreview = entry.blurPreview;
  out.revision = mediaRevision(record.path);
  out.thumbnailName = thumb::detachedThumbnailName(record.path);
  out.meta = packed;
  out.name = record.name;
  out.displayName = entry.vaultName;
  if (entry.favorite) out.favorite = true;
  return out;
}

struct TimestampChunk {
  AlbumMeta album;
  std::vector<std::pair<std::unique_ptr<TimestampTarget>, std::optional<QByteArray>>> originals;
  std::vector<DetachedMediaEntry> updated;
};

void writeTimestampRows(const QString &dir, const AlbumMeta &album) { writeSelected(dir, album.files.keys(), album); }

std::vector<DetachedMediaEntry> persistTimestampChunk(const QString &dir, TimestampChunk &chunk, AlbumMeta &album) {
  try {
    writeTimestampRows(dir, chunk.album);
  } catch (const Error &error) {
    QStringList errors;
    for (auto it = chunk.originals.rbegin(); it != chunk.originals.rend(); ++it) {
      try {
        it->first->restore(it->second, it->first->written);
      } catch (const Error &e) {
        errors << e.message();
      }
    }
    if (errors.isEmpty()) throw;
    fail(QStringLiteral("%1; date restore failed: %2").arg(error.message(), errors.join(QStringLiteral("; "))));
  }
  album = chunk.album;
  return std::move(chunk.updated);
}

std::vector<DetachedMediaEntry> commitTimestampUpdates(const QString &dir, const std::vector<PreparedTimestamp> &prepared, qint64 timestamp, std::optional<QString> failure) {
  if (prepared.empty()) {
    if (failure) fail(*failure);
    return {};
  }
  auto guard = lockMetadataForWrite({dir});
  ensureDatabase(dir);
  QStringList selected;
  for (const auto &record : prepared) selected << record.name;
  AlbumMeta album = album_db::subset(albumMetaPath(dir), selected, false);
  const QString version = readSettings().metadataVersion();
  std::vector<DetachedMediaEntry> updated;
  std::optional<TimestampChunk> pending;
  const int batch = timestampBatchSize();
  auto setFailure = [&](const QString &error) {
    if (!failure) failure = error;
  };
  for (const PreparedTimestamp &record : prepared) {
    std::unique_ptr<TimestampTarget> target;
    try {
      target = TimestampTarget::open(record.path, record.ns);
    } catch (const Error &error) {
      setFailure(error.message());
      break;
    }
    if (!target->current(record.source)) {
      setFailure(QStringLiteral("Media changed while updating its date: %1").arg(record.path));
      continue;
    }
    target->written = timestampValue(record.path, timestamp);
    const QByteArray &written = target->written;
    const xattr::Result previous = target->raw();
    if (previous.permissionDenied()) {
      // Write-only files can allow setting xattrs but forbid reading the old bytes. Commit their JSON first; a
      // failed atomic xattr set leaves the old attribute intact, so restore only the JSON.
      if (pending) {
        auto entries = persistTimestampChunk(dir, *pending, album);
        updated.insert(updated.end(), entries.begin(), entries.end());
        pending.reset();
      }
      AlbumMeta next = album;
      DetachedMediaEntry entry = applyTimestampMetadata(next, record, timestamp, version);
      writeTimestampRows(dir, next);
      QString changeError;
      if (target->current(record.source)) {
        try {
          target->writeRaw(written);
        } catch (const Error &e) {
          changeError = e.message();
        }
      } else {
        changeError = QStringLiteral("Media changed while updating its date: %1").arg(record.path);
      }
      if (!changeError.isEmpty()) {
        try {
          writeTimestampRows(dir, album);
        } catch (const Error &restore) {
          fail(QStringLiteral("%1; metadata restore failed: %2").arg(changeError, restore.message()));
        }
        fail(changeError);
      }
      album = next;
      updated.push_back(entry);
      continue;
    }
    if (previous.error) {
      setFailure(QStringLiteral("os error %1").arg(previous.error));
      break;
    }
    try {
      target->writeRaw(written);
    } catch (const Error &error) {
      try {
        target->restore(previous.value, written);
        setFailure(error.message());
      } catch (const Error &restore) {
        setFailure(QStringLiteral("%1; date restore failed: %2").arg(error.message(), restore.message()));
      }
      break;
    }
    if (!target->current(record.source)) {
      const QString error = QStringLiteral("Media changed while updating its date: %1").arg(record.path);
      try {
        target->restore(previous.value, written);
        setFailure(error);
        continue;
      } catch (const Error &restore) {
        setFailure(QStringLiteral("%1; date restore failed: %2").arg(error, restore.message()));
        break;
      }
    }
    if (!pending) pending.emplace(TimestampChunk{album, {}, {}});
    pending->updated.push_back(applyTimestampMetadata(pending->album, record, timestamp, version));
    pending->originals.emplace_back(std::move(target), previous.value);
    if (int(pending->originals.size()) == batch) {
      auto entries = persistTimestampChunk(dir, *pending, album);
      updated.insert(updated.end(), entries.begin(), entries.end());
      pending.reset();
    }
  }
  if (pending) {
    auto entries = persistTimestampChunk(dir, *pending, album);
    updated.insert(updated.end(), entries.begin(), entries.end());
  }
  if (failure) fail(*failure);
  return updated;
}
}  // namespace
#endif

#ifdef Q_OS_WIN
// Windows ADS changes file times and needs its own pinned-stream transaction.
static std::vector<DetachedMediaEntry> setMediaTimestampLegacy(const QString &dir, const QStringList &names, qint64 timestamp) {
  if (names.isEmpty()) return {};
  for (const QString &name : names) validateFileName(name);
  struct Prepared {
    QString name, path, source, meta;
  };
  std::vector<Prepared> prepared;
  std::optional<QString> failure;
  for (const QString &name : names) {
    const QString path = path::join(dir, name);
    try {
      const auto source = sourceRevision(path);
      if (!source) fail(QStringLiteral("%1 does not exist").arg(path));
      const QString meta = getFileMetadataCached(path);
      unpackFileMeta(meta);
      writeEmbeddedTimestamp(path, timestamp);
      prepared.push_back({name, path, *source, meta});
    } catch (const Error &error) {
      failure = error.message();
      break;
    }
  }
  std::vector<DetachedMediaEntry> updated;
  for (const Prepared &item : prepared) {
    if (sourceRevision(item.path) != item.source) {
      if (!failure) failure = QStringLiteral("Media changed during date update");
      continue;
    }
    const FileMetaEntry saved = updateFileMeta(dir, item.name, [&](FileMetaEntry &entry) {
      DetachedFileMeta meta = entry.meta && validPackedMeta(*entry.meta) ? unpackFileMeta(*entry.meta) : unpackFileMeta(item.meta);
      const auto embedded = readEmbeddedTimestamp(item.path);
      meta.s = embedded ? embedded : std::optional(timestamp);
      entry.metaEmbeddedTimestamp = embedded;
      entry.metaEmbeddedRevision = embeddedTimestampStampIfCurrent(item.path, embedded);
      entry.meta = meta.pack();
      entry.metaSourceRevision = item.source;
      entry.metaSettingsVersion = readSettings().metadataVersion();
      entry.metaFailed = false;
      entry.metaFailedSourceRevision.reset();
    });
    DetachedMediaEntry out;
    out.blurPreview = saved.blurPreview;
    out.revision = mediaRevision(item.path);
    out.thumbnailName = thumb::detachedThumbnailName(item.path);
    out.meta = *saved.meta;
    out.name = item.name;
    out.favorite = saved.favorite;
    out.displayName = saved.vaultName;
    updated.push_back(out);
  }
  if (failure) fail(*failure);
  return updated;
}
#endif

std::vector<DetachedMediaEntry> setMediaTimestamp(const QString &albumPath, const QStringList &names, qint64 timestamp) {
  if (!supportedTimestamp(timestamp)) fail(QStringLiteral("Date exceeds the supported timestamp range"));
  if (!isDir(albumPath)) fail(QStringLiteral("%1 is not a directory").arg(albumPath));
#ifdef Q_OS_WIN
  return setMediaTimestampLegacy(albumPath, names, timestamp);
#else
  for (const QString &name : names) validateFileName(name);
  auto [prepared, failure] = prepareTimestampUpdates(albumPath, names);
  return commitTimestampUpdates(albumPath, prepared, timestamp, failure);
#endif
}

DetachedMediaEntry setMediaFavorite(const QString &path, bool favorite) {
  if (!exists(path)) fail(QStringLiteral("%1 does not exist").arg(path));
  const QString meta = getFileMetadataCached(path);
  const QString dir = dirOf(path), name = nameOf(path);
  updateFileMeta(dir, name, [&](FileMetaEntry &entry) {
    entry.favorite = favorite;
    if (!entry.meta || !validPackedMeta(*entry.meta)) entry.meta = meta;
  });
  DetachedMediaEntry out;
  if (auto entry = readFileEntry(path)) {
    out.blurPreview = entry->blurPreview;
    out.displayName = entry->vaultName;
  }
  out.revision = mediaRevision(path);
  out.thumbnailName = thumb::detachedThumbnailName(path);
  out.meta = meta;
  out.name = name;
  if (favorite) out.favorite = true;
  return out;
}

// --- Tests -------------------------------------------------------------------------------------------------------

AlbumMeta readAlbumMeta(const QString &dir) {
  if (exists(albumMetaPath(dir))) return album_db::read(albumMetaPath(dir));
  if (!exists(metaDir(dir))) return {};
  prepareDatabase(dir);
  return album_db::read(albumMetaPath(dir));
}

void updateAlbumMeta(const QString &dir, const std::function<void(AlbumMeta &)> &update) {
  auto guard = lockMetadataForWrite({dir});
  ensureDatabase(dir);
  AlbumMeta album = album_db::read(albumMetaPath(dir));
  update(album);
  album_db::replace(albumMetaPath(dir), album);
}

}  // namespace core::metadata
