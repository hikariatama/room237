#pragma once
#include "entry.h"
#include "settings.h"

namespace core::thumb {

inline const QString kThumbDir = QStringLiteral(".room237-thumb");
// Temporary files with this prefix are protected by the directory lease; older binaries don't take that lease.
inline const QString kTempPrefix = QStringLiteral(".room237-thumbnail-v2-");

QString maintenanceLockPath(const QString &thumbDir);
QString thumbPath(const QString &original, const QString &thumbDir);
QStringList deletedThumbnailPaths(const QString &name, const QString &thumbDir);
std::optional<QString> detachedThumbnailName(const QString &original);
void removeLegacyPreviewLocks(const QString &thumbDir);
bool isLegacyPreviewLock(const QString &name);
bool cachedPreviewsCurrent(const QString &path, const QString &thumbDir, const FileMetaEntry &entry, const QString &version);
bool cachedThumbCurrent(const QString &path, const QString &thumbDir, const FileMetaEntry &entry, const QString &version);
QString ensureThumb(const QString &path, const QString &thumbDir);
QString ensureThumbWithSettings(const QString &path, const QString &thumbDir, const AdvancedSettings &settings);
std::optional<QString> contentRevision(const QString &path);
QString sourceHash(const QString &path);

}  // namespace core::thumb
