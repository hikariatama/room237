// Maintenance commands of the debug panel, and the one-way import of the 6.x profile.
#include "maintenance.h"

#include "album.h"
#include "album_db.h"
#include "catalog.h"
#include "constants.h"
#include "metadata.h"
#include "thumb.h"
#include "util.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtConcurrent>
#include <atomic>

namespace core::maintenance {

namespace {
void clearThumbnailFiles(const QString &thumbDir) {
  if (!exists(thumbDir)) return;
  auto lease = FileLock::acquire(thumb::maintenanceLockPath(thumbDir), std::chrono::seconds(readSettings().ffmpeg.timeoutSecs * 2 + 5), std::chrono::milliseconds(5));
  // Generators hold the lease shared, so none is running here.
  for (const DirEntry &entry : readDir(thumbDir)) {
    // The directory's own leases stay. Unleased temps from older binaries may still have an active owner.
    if ((entry.name.startsWith(QLatin1String(".room237-")) && entry.name.endsWith(QLatin1String(".lock"))) || (entry.name.startsWith(QLatin1String(".room237-thumbnail-")) && !entry.name.startsWith(thumb::kTempPrefix))) continue;
    const QString p = path::join(thumbDir, entry.name);
    if (entry.isDir) removeTree(p);
    else removeFile(p);
  }
}
QStringList mediaIn(const QString &album) {
  const QSet<QString> hidden = metadata::hiddenHeicNames(album);
  QStringList files;
  for (const DirEntry &entry : readDir(album))
    if (!hidden.contains(entry.name) && constants::isMedia(entry.name)) files << path::join(album, entry.name);
  return files;
}
}  // namespace

quint64 rebuildThumbnails(const QString &root) {
  if (!isDir(root)) fail(QStringLiteral("%1 is not a directory").arg(root));
  quint64 written = 0;
  for (const QString &album : album::walkAlbumPaths(root)) {
    const QString thumbDir = path::join(album, thumb::kThumbDir);
    clearThumbnailFiles(thumbDir);
    createDerivedDir(thumbDir);
    std::atomic<quint64> done {0};
    QtConcurrent::blockingMap(mediaIn(album), [&](const QString &file) {
      try {
        thumb::ensureThumb(file, thumbDir);
        ++done;
      } catch (const std::exception &) {
      }
    });
    written += done;
  }
  return written;
}

quint64 rebuildMetadata(const QString &root) {
  if (!isDir(root)) fail(QStringLiteral("%1 is not a directory").arg(root));
  quint64 written = 0;
  for (const QString &album : album::walkAlbumPaths(root)) {
    QSet<QString> visited;
    for (const QString &file : mediaIn(album)) {
      if (visited.contains(file)) continue;
      visited.insert(file);
      metadata::getFileMetadata(file);
      ++written;
    }
  }
  return written;
}

quint64 resetDuplicates(const QString &root) {
  if (!isDir(root)) fail(QStringLiteral("%1 is not a directory").arg(root));
  quint64 removed = 0;
  for (const QString &album : album::walkAlbumPaths(root)) {
    metadata::prepareDatabase(album);
    auto guard = metadata::lockMetadataForWrite({album});
    if (album_db::clearIgnores(path::join(album, QStringLiteral(".room237-metadata/album.sqlite")))) ++removed;
  }
  return removed;
}

quint64 clearRoom237Artifacts(const QString &root) {
  if (!isDir(root)) fail(QStringLiteral("%1 is not a directory").arg(root));
  quint64 cleared = 0;
  QStringList dirs = album::walkAlbumPaths(root);
  dirs << root;
  for (const QString &dir : dirs) {
    {
      auto guard = metadata::lockMetadataForWrite({dir});
      const QString metaDir = path::join(dir, metadata::kMetaDir);
      if (exists(metaDir)) {
        try {
          removeTree(metaDir);
          ++cleared;
        } catch (const Error &e) {
          qWarning("failed to remove %s: %s", qUtf8Printable(metaDir), qUtf8Printable(e.message()));
        }
      }
      const QString legacy = path::join(dir, QStringLiteral(".room237-meta.json"));
      if (exists(legacy)) {
        try {
          removeFile(legacy);
          ++cleared;
        } catch (const Error &e) {
          qWarning("failed to remove %s: %s", qUtf8Printable(legacy), qUtf8Printable(e.message()));
        }
      }
      // A thumbnail worker persists metadata while holding its preview lock. Release metadata locks before waiting on
      // any preview lock.
    }
    const QString thumbDir = path::join(dir, thumb::kThumbDir);
    if (exists(thumbDir)) {
      try {
        clearThumbnailFiles(thumbDir);
        ++cleared;
      } catch (const Error &e) {
        qWarning("failed to remove %s: %s", qUtf8Printable(thumbDir), qUtf8Printable(e.message()));
      }
    }
  }
  catalog::clear(root);
  return cleared;
}

// One-way import of the Tauri profile. The old profile is only read, so the old app keeps working. Every step can
// repeat after a crash: files appear through atomic renames and the marker comes last.
namespace {
bool writeAtomic(const QString &target, const QByteArray &bytes) {
  const QString temporary = path::join(path::parent(target), path::stem(target) + QStringLiteral(".migrating"));
  try {
    writeAll(temporary, bytes);
    syncFile(temporary);
    renameReplace(temporary, target);
    return true;
  } catch (const Error &) {
    return false;
  }
}
std::optional<QJsonObject> legacyUiState(const QString &legacy) {
  QFile file(path::join(legacy, QStringLiteral("state.json")));
  if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
  const QJsonDocument store = QJsonDocument::fromJson(file.readAll());
  if (!store.isObject()) return std::nullopt;
  const QJsonValue inner = store.object().value(QLatin1String("room237-storage"));
  if (inner.isUndefined()) return std::nullopt;
  if (inner.isString()) {
    const QJsonDocument state = QJsonDocument::fromJson(inner.toString().toUtf8());
    if (!state.isObject()) return std::nullopt;
    return state.object();
  }
  if (!inner.isObject()) return std::nullopt;
  return inner.toObject();
}
}  // namespace

MigrationOutcome migrateProfile(const QString &profile, const QString &legacy) {
  const QString marker = path::join(profile, QStringLiteral("migration-tauri.json"));
  if (isFile(marker)) return {MigrationOutcome::AlreadyDone};
  if (!isDir(legacy) || legacy == profile) return {MigrationOutcome::NoLegacyProfile};
  try {
    createDirs(profile);
  } catch (const Error &) {
  }
  MigrationOutcome outcome{MigrationOutcome::Imported};
  const QString settingsTarget = path::join(profile, QStringLiteral("settings.json"));
  if (!exists(settingsTarget)) {
    // Corrupt settings stay behind: the core reports them instead of silently losing privacy flags.
    QFile source(path::join(legacy, QStringLiteral("settings.json")));
    if (source.open(QIODevice::ReadOnly)) {
      const QByteArray bytes = source.readAll();
      if (QJsonDocument::fromJson(bytes).isObject() && writeAtomic(settingsTarget, bytes)) outcome.settings = true;
    }
  }
  const QString stateTarget = path::join(profile, QStringLiteral("ui-state.json"));
  if (!exists(stateTarget))
    if (const auto state = legacyUiState(legacy)) outcome.state = writeAtomic(stateTarget, QJsonDocument(*state).toJson(QJsonDocument::Indented));
  const QJsonObject record{{QStringLiteral("source"), legacy}, {QStringLiteral("settings"), outcome.settings}, {QStringLiteral("state"), outcome.state}};
  writeAtomic(marker, QJsonDocument(record).toJson(QJsonDocument::Compact));
  return outcome;
}

}  // namespace core::maintenance
