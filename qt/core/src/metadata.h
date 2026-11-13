#pragma once
#include "album_tree.h"
#include "entry.h"
#include "fs.h"
#include "settings.h"

#include <QJsonObject>
#include <functional>
#include <memory>
#include <mutex>

namespace core::metadata {

inline const QString kMetaDir = QStringLiteral(".room237-metadata");
QString metaDir(const QString &dir);
QString albumMetaPath(const QString &dir);
bool supportedTimestamp(qint64 timestamp);

// Revisions. A source revision changes with the original; a media revision also with its preview.
std::optional<QString> sourceRevision(const QString &path);
std::optional<QString> sourceRevisionFromInfo(const FileInfo &info);
std::pair<QString, QString> mediaRevisionParts(const QString &path);
QString mediaRevision(const QString &path);

struct ObservedMedia {
  QString path, revision;
  static std::vector<ObservedMedia> fromJson(const QJsonValue &value);
};
void validateMediaRevisions(const std::vector<ObservedMedia> &observed);

// Background writes from preload workers join batched transactions; foreground writes commit immediately.
class BackgroundMetadataWrites {
public:
  BackgroundMetadataWrites();
  ~BackgroundMetadataWrites();
private:
  bool previous_;
};

std::unique_ptr<FileLock> lockAlbumMetadata(const QString &dir);
class MetadataWriteGuard {
public:
  std::vector<std::unique_ptr<FileLock>> leases;
  std::unique_ptr<album_tree::TreeLease> tree;
  QStringList directories;
  bool covers(const QString &canonicalDir) const { return directories.contains(canonicalDir); }
};
MetadataWriteGuard lockMetadataForWrite(const QStringList &dirs);
std::unique_ptr<MetadataWriteGuard> tryLockMetadataForWrite(const QString &dir);
// The caller then holds mediaMoveLock through the returned lock.
std::pair<std::unique_lock<std::mutex>, MetadataWriteGuard> lockMetadataForMoveWithTree(const QStringList &dirs, const album_tree::TreeLease &tree);
std::pair<std::unique_lock<std::mutex>, MetadataWriteGuard> lockMetadataForMove(const QStringList &dirs);

AlbumMeta readSelectedMetadata(const QString &dir, const QStringList &names, bool related);
AlbumMeta readSelectedWithGuard(const QString &dir, const QStringList &names, bool related, const MetadataWriteGuard &guard);
void removeDeletedMetadataWithGuard(const QString &dir, const QStringList &names, const MetadataWriteGuard &guard);
void prepareDatabase(const QString &dir);
void ensureDatabase(const QString &dir);
std::optional<FileMetaEntry> readFileEntry(const QString &path);

FileMetaEntry updateFileMeta(const QString &dir, const QString &name, const std::function<void(FileMetaEntry &)> &update);
FileMetaEntry updateFileMetaIfSourceCurrent(const QString &path, const QString &expectedSource, std::function<void(FileMetaEntry &)> update);
FileMetaEntry updateFileMetaIfCurrent(const QString &path, const QString &expectedRevision, const std::optional<QString> &expectedSource, std::function<void(FileMetaEntry &)> update);

bool hashCacheCurrent(const QString &path, const FileMetaEntry &entry);
bool metadataCacheCurrent(const QString &path, const FileMetaEntry &entry);
bool metadataCacheCurrentWithSettings(const QString &path, const FileMetaEntry &entry, const AdvancedSettings &settings);
bool metadataContentCacheCurrent(const QString &path, const FileMetaEntry &entry, const AdvancedSettings &settings);
void markThumbFailed(const QString &path, const QString &expectedSource, const QString &processingVersion);
void markMetaFailed(const QString &path, const QString &expectedSource, const QString &processingVersion);
void markHashFailedIfCurrent(const QString &path, const QString &expectedRevision, const HashConfig &config, const QString &processingVersion);
bool isThumbFailed(const QString &path);
bool isMetaFailed(const QString &path);

struct CachedHash {
  QString hash;
  std::optional<QString> version;
  std::optional<quint32> bits;
};
std::optional<CachedHash> loadAlbumFileHash(const QString &path);
void writeAlbumFileHash(const QString &path, const QString &hashB64, const QString &hashVersion, quint32 hashBits, const QString &expectedRevision);
void writeThumbState(const QString &path, const QString &thumbVersion, const QString &expectedSource, std::optional<QString> sourceHash, std::optional<QString> hashRevision, std::optional<QString> blurPreview);

struct StoredMetadata {
  QString meta;
  bool favorite = false;
};
StoredMetadata getMetadataWithFavorite(const QString &path);
StoredMetadata getMetadataWithFavoriteWithSettings(const QString &path, const AdvancedSettings &settings);
QString getFileMetadata(const QString &path);
QString getFileMetadataWithSettings(const QString &path, const AdvancedSettings &settings);
QString getFileMetadataCached(const QString &path);
QString getFileMetadataCachedWithSettings(const QString &path, const AdvancedSettings &settings);
void invalidateMediaContent(const QString &path);

std::optional<qint64> readEmbeddedTimestamp(const QString &path);
void writeEmbeddedTimestamp(const QString &path, qint64 timestamp);  // Tests and Windows.

// HEIC companions recorded by older builds.
// Images among plain names, and among encrypted ones by the kind their sealed record names.
bool isImageMedia(const QString &album, const QString &name);
bool heicHiddenByEntry(const QString &album, const FileMetaEntry &entry);
QSet<QString> hiddenHeicNames(const QString &album);
bool heicBindingCurrent(const QString &dir, const QString &name, const HeicConversion &binding);

// Media move journals: the metadata rows a move changed, to roll back after a crash.
struct MoveMetadataRow {
  QString name;
  std::optional<FileMetaEntry> before, after;
};
struct MoveMetadataUndo {
  std::vector<MoveMetadataRow> source, target;
  void validate() const;
  void validateCurrent(const AlbumMeta &source, const AlbumMeta &target) const;
  QString toJson() const;
  static MoveMetadataUndo fromJson(const QJsonObject &object);
};
void validateMoveMetadataUndoWithGuard(const QString &source, const QString &target, const MoveMetadataUndo &undo, const MetadataWriteGuard &guard);
void restoreMoveMetadataWithHooks(const QString &source, const QString &target, const MoveMetadataUndo &undo, const MetadataWriteGuard &guard, const std::function<void(const QString &)> &checkpoint);
void transferMediaState(const QString &source, const QString &target, const std::vector<std::pair<QString, QString>> &moves, const MetadataWriteGuard &guard, bool transferFields,
                        const std::function<void(const MoveMetadataUndo &)> &prepare, const std::function<void(const QString &)> &checkpoint);

std::vector<DetachedMediaEntry> setMediaTimestamp(const QString &albumPath, const QStringList &names, qint64 timestamp);
DetachedMediaEntry setMediaFavorite(const QString &path, bool favorite);
void deleteMediaIfUnchanged(const QString &path, const QString &revision, const std::vector<ObservedMedia> &observed);

// Tests.
AlbumMeta readAlbumMeta(const QString &dir);
void updateAlbumMeta(const QString &dir, const std::function<void(AlbumMeta &)> &update);
}  // namespace core::metadata
