#pragma once
#include "entry.h"

#include <QJsonArray>
#include <QJsonObject>

namespace core::album {

bool isAlbumDir(const QString &path);
QStringList walkAlbumPaths(const QString &root);

struct RenamedAlbumResult {
  QString oldPath, newPath, oldRelativePath, newRelativePath;
  std::optional<QString> parent;
  QString name;
  QJsonObject toJson() const;
};

struct IncomingFile {
  QString name;
  std::optional<QString> sourcePath;
  std::optional<QByteArray> data;
  static std::vector<IncomingFile> fromJson(const QJsonValue &value);
};
struct ImportResult {
  std::vector<DetachedMediaEntry> added;
  std::vector<std::pair<QString, QString>> failed;  // name, error
  QJsonObject toJson() const;
};

QJsonArray getAlbumsDetached(const QString &rootDir);
QJsonArray getAlbumMedia(const QString &dir);
QJsonArray listFavorites(const QString &rootDir);
void deleteAlbum(const QString &rootDir, const QString &albumId);
RenamedAlbumResult renameAlbum(const QString &rootDir, const QString &albumId, const QString &newName);
RenamedAlbumResult moveAlbum(const QString &rootDir, const QString &albumId, const std::optional<QString> &newParentId);
quint64 getAlbumSize(const QString &dir);
QStringList moveMediaBatch(const QString &source, const QString &target, const QStringList &media);
QString moveMedia(const QString &source, const QString &target, const QString &media);
// Tests: checkpoints may throw to simulate a crash at a named phase.
QStringList moveMediaBatchWithHook(const QString &source, const QString &target, const QStringList &media, bool moveArtifacts, const std::function<void(const QString &, const QString &, const QString &)> &checkpoint);
DetachedMediaEntry registerNewMedia(const QString &albumPath, const QString &mediaName);
ImportResult addMediaFiles(const QString &dir, const std::vector<IncomingFile> &files);

}  // namespace core::album
