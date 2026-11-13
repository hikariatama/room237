#pragma once
// Journaled deletion: originals are staged inside the album, a commit record is published, then they are removed.
// Recovery finishes a committed deletion or restores an uncommitted one.
#include "metadata.h"

#include <QJsonObject>

namespace core::media_delete {

inline constexpr int kDeleteBatchLimit = 128;

struct DeleteTarget {
  QString name;
  std::optional<QString> revision;
  static std::vector<DeleteTarget> fromJson(const QJsonValue &value);
};
struct DeleteBatchResult {
  QStringList deleted;
  std::optional<QString> error;
  QJsonObject toJson() const;
};

std::unique_ptr<FileLock> lockMaintenance(const QString &album);
void recoverAlbum(const QString &album);
void deleteMedia(const QString &path);
DeleteBatchResult deleteMediaBatch(const QString &albumPath, const std::vector<DeleteTarget> &targets, const std::vector<metadata::ObservedMedia> &observed);
void deleteIfUnchanged(const QString &path, const QString &revision, const std::vector<metadata::ObservedMedia> &observed);
DeleteBatchResult nativeDeleteBatch(const QString &albumPath, const std::vector<DeleteTarget> &targets);
// Tests: checkpoints may throw to simulate a crash at a named phase.
DeleteBatchResult deleteBatchWithMode(const QString &album, const std::vector<DeleteTarget> &targets, const std::vector<metadata::ObservedMedia> &observed, bool requireRetained,
                                      const std::function<void(const QString &)> &checkpoint);

}  // namespace core::media_delete
