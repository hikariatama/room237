#include "media_delete.h"

#include "album_tree.h"
#include "constants.h"
#include "json.h"
#include "media_move.h"
#include "thumb.h"
#include "util.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#ifndef Q_OS_WIN
#include <cerrno>
#include <unistd.h>
#endif

namespace core::media_delete {

namespace {
const QString kPrefix = QStringLiteral(".room237-delete-");
const QString kJournal = QStringLiteral("journal.json");
constexpr qint64 kJournalLimit = 1024 * 1024;

void validateMediaName(const QString &name) {
  validateFileName(name);
  if (!constants::isMedia(name)) fail(QStringLiteral("Expected a supported media file"));
}

struct Entry {
  QString name, revision;
};
struct Journal {
  int version = 1;
  bool committed = false;
  std::vector<Entry> files;
  QString toJson() const {
    QStringList items;
    for (const Entry &entry : files) items << json::Object().str("name", entry.name).str("revision", entry.revision).compact();
    return json::Object().num("version", version).flag("committed", committed).raw("files", QLatin1Char('[') + items.join(QLatin1Char(',')) + QLatin1Char(']')).compact();
  }
};

std::optional<QString> ownedRevision(const QString &p) { return metadata::sourceRevisionFromInfo(lstat(p)); }

void syncDirectory(const QString &p) {
#ifndef Q_OS_WIN
  syncDir(p);
#else
  Q_UNUSED(p)
#endif
}

void writeJournal(const QString &staging, const Journal &journal) {
  const QByteArray bytes = journal.toJson().toUtf8();
  if (bytes.size() > kJournalLimit) fail(QStringLiteral("Deletion journal is too large"));
  atomicWriteExisting(path::join(staging, kJournal), bytes);
  syncDirectory(staging);
}

void validateJournal(const Journal &journal) {
  if (journal.version != 1 || journal.files.empty()) fail(QStringLiteral("Unsupported deletion journal"));
  QSet<QString> names;
  for (const Entry &entry : journal.files) {
    validateMediaName(entry.name);
    if (entry.revision.isEmpty() || names.contains(entry.name)) fail(QStringLiteral("Invalid deletion journal entry"));
    names.insert(entry.name);
  }
}

void removeOptionalCache(const QString &p) {
  const FileInfo info = lstat(p);
  if (!info.exists) return;  // Includes names this filesystem rejects as too long.
  if (info.isFile || info.isSymlink) return removeFile(p);
  fail(QStringLiteral("Derived artifact is not a file: %1").arg(p));
}

void cleanupDeletedArtifacts(const QString &album, const Journal &journal, const metadata::MetadataWriteGuard &guard) {
  const QString thumbs = path::join(album, thumb::kThumbDir), metadataDir = path::join(album, metadata::kMetaDir);
  for (const QString &directory : {thumbs, metadataDir}) {
    const FileInfo info = lstat(directory);
    if (info.exists && !info.isDir) fail(QStringLiteral("Derived directory is not a regular directory: %1").arg(directory));
  }
  QStringList names;
  for (const Entry &entry : journal.files) {
    if (lstat(path::join(album, entry.name)).exists) continue;  // Preserve a replacement and its metadata/cache.
    for (const QString &preview : thumb::deletedThumbnailPaths(entry.name, thumbs)) removeOptionalCache(preview);
    removeOptionalCache(path::join(metadataDir, entry.name + QStringLiteral(".meta")));
    names << entry.name;
  }
  // Remove legacy rows first so a restart cannot migrate them back into JSON.
  metadata::removeDeletedMetadataWithGuard(album, names, guard);
  for (const QString &directory : {thumbs, metadataDir})
    if (isDir(directory)) syncDirectory(directory);
}

void removeEmptyDir(const QString &p) {
#ifndef Q_OS_WIN
  if (::rmdir(QFile::encodeName(p).constData()) != 0) fail(QString::fromLocal8Bit(strerror(errno)));
#else
  if (!QDir().rmdir(p)) fail(QStringLiteral("Cannot remove %1").arg(p));
#endif
}

void finishOrRollback(const QString &album, const QString &staging, const Journal &journal, const metadata::MetadataWriteGuard &guard, const std::function<void(const QString &)> &checkpoint) {
  validateJournal(journal);
  QStringList errors;
  for (const Entry &entry : journal.files) {
    const QString staged = path::join(staging, entry.name), original = path::join(album, entry.name);
    const FileInfo info = lstat(staged);
    if (!info.exists) {
      // The file was never staged or has already been restored. A later external replacement belongs to its writer.
      if (!journal.committed && !lstat(original).exists) errors << QStringLiteral("Missing original during deletion recovery: %1: No such file or directory (os error 2)").arg(entry.name);
      continue;
    }
    if (!(info.isFile || info.isSymlink) || ownedRevision(staged) != entry.revision) {
      errors << QStringLiteral("Staged media changed; preserving %1").arg(entry.name);
      continue;
    }
    try {
      if (journal.committed) removeFile(staged);
      else {
        QString error;
        if (!renameNoClobber(staged, original, &error)) fail(error);
      }
    } catch (const Error &error) {
      errors << QStringLiteral("%1: %2").arg(entry.name, error.message());
    }
  }
  if (!errors.isEmpty()) fail(errors.join(QStringLiteral("; ")));
  syncDirectory(album);
  syncDirectory(staging);
  if (journal.committed) {
    checkpoint(QStringLiteral("after_media_cleanup"));
    cleanupDeletedArtifacts(album, journal, guard);
    checkpoint(QStringLiteral("after_artifact_cleanup"));
  }
  bool unknown = false;
  for (const DirEntry &entry : readDir(staging)) unknown |= entry.name != kJournal;
  if (unknown) {
    const QString name = path::fileName(staging);
    if (!name.startsWith(kPrefix)) fail(QStringLiteral("Invalid staging directory"));
    const QString quarantine = path::join(album, QStringLiteral(".room237-orphan-delete-") + name.mid(kPrefix.size()));
    QString error;
    if (!renameNoClobber(staging, quarantine, &error)) fail(error);
    qWarning("Preserved unknown deletion staging files in %s", qUtf8Printable(quarantine));
    return syncDirectory(album);
  }
  removeFile(path::join(staging, kJournal));
  removeEmptyDir(staging);
  syncDirectory(album);
}

QStringList stagingDirs(const QString &album) {
  QStringList dirs;
  for (const DirEntry &entry : readDir(album))
    if (entry.name.startsWith(kPrefix) && entry.isDir) dirs << path::join(album, entry.name);
  return dirs;
}

Journal parseJournal(const QByteArray &bytes, const QString &staging) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) fail(QStringLiteral("Deletion journal %1: %2").arg(staging, error.errorString()));
  const QJsonObject o = document.object();
  try {
    Journal journal;
    journal.version = int(json::unsignedField(o.value(QLatin1String("version")), "version", 255));
    journal.committed = json::boolField(o.value(QLatin1String("committed")), "committed");
    const QJsonValue files = o.value(QLatin1String("files"));
    if (!files.isArray()) fail(QStringLiteral("missing field `files`"));
    for (const QJsonValue &file : files.toArray()) {
      const QJsonObject entry = file.toObject();
      journal.files.push_back({json::stringField(entry.value(QLatin1String("name")), "name"), json::stringField(entry.value(QLatin1String("revision")), "revision")});
    }
    return journal;
  } catch (const Error &e) {
    fail(QStringLiteral("Deletion journal %1: %2").arg(staging, e.message()));
  }
}

void recoverLocked(const QString &album, const metadata::MetadataWriteGuard &guard) {
  for (const QString &staging : stagingDirs(album)) {
    if (listDir(staging).isEmpty()) {
      removeEmptyDir(staging);
      syncDirectory(album);
      continue;
    }
    const QString journalPath = path::join(staging, kJournal);
    if (!lstat(journalPath).isFile) fail(QStringLiteral("Deletion journal is not a regular file"));
    const QByteArray bytes = readAll(journalPath, kJournalLimit + 1);
    if (bytes.size() > kJournalLimit) fail(QStringLiteral("Deletion journal is too large"));
    finishOrRollback(album, staging, parseJournal(bytes, staging), guard, [](const QString &) {});
  }
}

void deleteWith(const QString &file, const std::optional<QString> &revision, const std::vector<metadata::ObservedMedia> &observed) {
  const QString name = path::fileName(file);
  if (name.isEmpty()) fail(QStringLiteral("Invalid media name"));
  const QString album = path::parent(file);
  if (album.isEmpty()) fail(QStringLiteral("Missing media album"));
  const DeleteBatchResult result = deleteBatchWithMode(album, {{name, revision}}, observed, true, [](const QString &) {});
  if (result.error) fail(*result.error);
}
}  // namespace

std::vector<DeleteTarget> DeleteTarget::fromJson(const QJsonValue &value) {
  if (!value.isArray()) fail(QStringLiteral("targets: invalid type, expected a sequence"));
  std::vector<DeleteTarget> targets;
  for (const QJsonValue &item : value.toArray()) {
    const QJsonObject o = item.toObject();
    DeleteTarget target;
    target.name = json::stringField(o.value(QLatin1String("name")), "name");
    const QJsonValue revision = o.value(QLatin1String("revision"));
    if (!revision.isNull() && !revision.isUndefined()) target.revision = json::stringField(revision, "revision");
    targets.push_back(target);
  }
  return targets;
}

QJsonObject DeleteBatchResult::toJson() const { return {{QStringLiteral("deleted"), QJsonArray::fromStringList(deleted)}, {QStringLiteral("error"), error ? QJsonValue(*error) : QJsonValue()}}; }

std::unique_ptr<FileLock> lockMaintenance(const QString &album) {
  const QString thumbs = path::join(album, thumb::kThumbDir);
  try {
    createDir(thumbs);
  } catch (const Error &) {
    if (!isDir(thumbs)) throw;
  }
  return FileLock::acquire(thumb::maintenanceLockPath(thumbs), std::chrono::seconds(readSettings().ffmpeg.timeoutSecs), std::chrono::milliseconds(5));
}

void recoverAlbum(const QString &album) {
  auto tree = album_tree::read({album});
  media_move::recoverAlbum(album);
  if (stagingDirs(album).isEmpty()) return;
  auto maintenance = lockMaintenance(album);
  auto [moving, guard] = metadata::lockMetadataForMoveWithTree({album}, *tree);
  recoverLocked(album, guard);
}

void deleteMedia(const QString &file) { deleteWith(file, std::nullopt, {}); }

DeleteBatchResult deleteMediaBatch(const QString &albumPath, const std::vector<DeleteTarget> &targets, const std::vector<metadata::ObservedMedia> &observed) {
  return deleteBatchWithMode(albumPath, targets, observed, true, [](const QString &) {});
}

void deleteIfUnchanged(const QString &file, const QString &revision, const std::vector<metadata::ObservedMedia> &observed) { deleteWith(file, revision, observed); }

// Ordinary gallery deletion checks reviewed revisions under the same leases as duplicate deletion, but may
// intentionally delete the whole selection.
DeleteBatchResult nativeDeleteBatch(const QString &albumPath, const std::vector<DeleteTarget> &targets) {
  for (const DeleteTarget &target : targets)
    if (!target.revision || target.revision->isEmpty()) fail(QStringLiteral("Every selected file needs its reviewed revision"));
  return deleteBatchWithMode(albumPath, targets, {}, false, [](const QString &) {});
}

DeleteBatchResult deleteBatchWithMode(const QString &requestedAlbum, const std::vector<DeleteTarget> &targets, const std::vector<metadata::ObservedMedia> &observed, bool requireRetained,
                                      const std::function<void(const QString &)> &checkpoint) {
  if (targets.empty()) return {};
  if (int(targets.size()) > kDeleteBatchLimit) fail(QStringLiteral("Too many files in one deletion batch"));
  bool guarded = false;
  for (const DeleteTarget &target : targets) guarded |= target.revision.has_value();
  QHash<QString, std::optional<QString>> seen;
  std::vector<DeleteTarget> unique;
  for (const DeleteTarget &target : targets) {
    validateMediaName(target.name);
    if (guarded && (!target.revision || target.revision->isEmpty())) fail(QStringLiteral("Every duplicate deletion needs its observed revision"));
    if (seen.contains(target.name)) {
      if (seen.value(target.name) != target.revision) fail(QStringLiteral("Conflicting revisions for one deletion target"));
    } else {
      seen.insert(target.name, target.revision);
      unique.push_back(target);
    }
  }
  if ((guarded && requireRetained && observed.empty()) || (!guarded && !observed.empty())) fail(QStringLiteral("Duplicate deletion needs retained observations and revisions"));
  const QString album = canonical(requestedAlbum);
  auto tree = album_tree::read({album});
  media_move::recoverAlbum(album);
  auto isSelected = [&](const QString &file) {
    const QString name = path::fileName(file);
    const QString parent = path::parent(file);
    return !name.isEmpty() && seen.contains(name) && !parent.isEmpty() && tryCanonical(parent) == album;
  };
  // Producers take the shared maintenance lease before their per-file locks. Wait for them before reserving the move
  // mutex or metadata leases.
  auto maintenance = lockMaintenance(album);
  auto [moving, guard] = metadata::lockMetadataForMoveWithTree({album}, *tree);
  recoverLocked(album, guard);
  if (guarded && requireRetained) {
    bool retained = false;
    for (const auto &item : observed) retained |= !isSelected(item.path);
    if (!retained) fail(QStringLiteral("Duplicate deletion must retain an observed file"));
    for (const auto &item : observed)
      if (item.revision.isEmpty() || !isFile(item.path) || metadata::mediaRevision(item.path) != item.revision) fail(QStringLiteral("Media changed; refresh duplicates before deleting."));
  }
  QStringList uniqueNames;
  for (const DeleteTarget &target : unique) uniqueNames << target.name;
  const AlbumMeta snapshot = metadata::readSelectedWithGuard(album, uniqueNames, true, guard);
  Journal journal;
  DeleteBatchResult result;
  QHash<QString, QString> selectedRegular;
  for (const DeleteTarget &target : unique) {
    const QString file = path::join(album, target.name);
    bool regular = false;
    QString revision;
    try {
      if (target.revision && metadata::mediaRevision(file) != *target.revision) fail(QStringLiteral("Media changed; refresh duplicates before deleting."));
      const FileInfo info = lstat(file);
      if (!info.exists) fail(QStringLiteral("No such file or directory (os error 2)"));
      if (!(info.isFile || info.isSymlink)) fail(QStringLiteral("Media is not a file"));
      regular = info.isFile;
      const auto owned = ownedRevision(file);
      if (!owned) fail(QStringLiteral("Media is unavailable"));
      revision = *owned;
    } catch (const Error &error) {
      result.error = error.message();
      break;
    }
    if (regular) selectedRegular.insert(target.name, revision);
    result.deleted << target.name;
    journal.files.push_back({target.name, revision});
  }
  if (journal.files.empty()) fail(result.error.value_or(QStringLiteral("No deletion targets")));
  QSet<QString> planned;
  for (const Entry &entry : journal.files) planned.insert(entry.name);
  std::vector<Entry> companions;
  // Index all selected outputs once; do not scan a large album per JPEG.
  for (auto it = snapshot.files.cbegin(); it != snapshot.files.cend(); ++it) {
    if (!it->heicConversion || !selectedRegular.contains(it->heicConversion->outputName)) continue;
    validateFileName(it.key());
    const QString source = path::join(album, it.key());
    if (it->heicConversion->outputRevision == selectedRegular.value(it->heicConversion->outputName) && path::hasExtension(source, {"heic"}) && lstat(source).isFile && ownedRevision(source) == it->heicConversion->sourceRevision) {
      if (planned.contains(it.key())) continue;
      planned.insert(it.key());
      companions.push_back({it.key(), it->heicConversion->sourceRevision});
    }
  }
  std::sort(companions.begin(), companions.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
  journal.files.insert(journal.files.end(), companions.begin(), companions.end());
  TempDir temporary(album, kPrefix);
  validateJournal(journal);
  writeJournal(temporary.path(), journal);
  syncDirectory(album);
  const QString staging = temporary.path();
  temporary.release();
  try {
    for (const Entry &entry : journal.files) {
      const QString original = path::join(album, entry.name), target = path::join(staging, entry.name);
      if (ownedRevision(original) != entry.revision) fail(QStringLiteral("Media changed before deletion"));
      QString error;
      if (!renameNoClobber(original, target, &error)) fail(error);
      if (ownedRevision(target) != entry.revision) fail(QStringLiteral("Media changed during deletion"));
      checkpoint(QStringLiteral("staged"));
    }
    syncDirectory(album);
    syncDirectory(staging);
    for (const auto &item : observed)
      if (!isSelected(item.path) && (item.revision.isEmpty() || !isFile(item.path) || metadata::mediaRevision(item.path) != item.revision)) fail(QStringLiteral("Retained duplicate changed during deletion"));
    checkpoint(QStringLiteral("before_commit"));
  } catch (const Error &error) {
    try {
      finishOrRollback(album, staging, journal, guard, checkpoint);
    } catch (const Error &rollback) {
      fail(QStringLiteral("%1; deletion rollback: %2").arg(error.message(), rollback.message()));
    }
    throw;
  }
  journal.committed = true;
  // An ambiguous durable commit is left for recovery; never remove staged originals when journal publication reports
  // an error.
  writeJournal(staging, journal);
  checkpoint(QStringLiteral("committed"));
  try {
    finishOrRollback(album, staging, journal, guard, checkpoint);
  } catch (const Error &error) {
    qWarning("Committed media deletion needs cleanup: %s", qUtf8Printable(error.message()));
  }
  return result;
}

}  // namespace core::media_delete
