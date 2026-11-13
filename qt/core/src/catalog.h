#pragma once
// Rebuildable library index. Sorting, filtering and inventory live on disk; a request returns at most one page,
// never a whole album or favorites collection.
#include "entry.h"

#include <QJsonArray>
#include <QJsonObject>

namespace core::catalog {

inline constexpr int kPageSize = 256;
inline const QString kDirectory = QStringLiteral(".room237-catalog");

struct PageQuery {
  QString rootDir;
  std::optional<QString> albumId;
  bool favoritesOnly = false;
  QString sortKey, sortDir;
  double randomSeed = 0;
  quint64 page = 0;
  std::optional<quint64> limitPages;
  static PageQuery fromJson(const QJsonValue &value);
};

struct PageItem {
  DetachedMediaEntry media;
  QString albumPath, albumId, albumName;
  QJsonObject toJson() const;
};
struct MediaPage {
  std::vector<PageItem> items;
  quint64 total = 0, page = 0;
  QJsonObject toJson() const;
};

QString albumId(const QString &root, const QString &album);
std::optional<QString> libraryRoot(const QString &album);
// Caller owns metadata admission; this derived index never locks metadata.
void publish(const QString &album, const QString &name, const FileMetaEntry &entry);
void forget(const QString &album, const QStringList &names);
int scanAlbum(const QString &root, const QString &album);
int scanAlbumIfNeeded(const QString &root, const QString &album, bool force);
void initialize(const QString &root);
void retainAlbums(const QString &root, const QStringList &albumIds);
void clear(const QString &root);
MediaPage queryPage(const PageQuery &query);

// Commands.
bool reconcileAlbum(const QString &rootDir, const QString &albumId);
void rescanInventory(const QString &rootDir, const std::optional<QString> &albumId);
void refreshCatalogPaths(const QString &rootDir, const QStringList &paths);
QString getInventoryPrint(PageQuery query);
MediaPage getMediaPage(const PageQuery &query);
QJsonObject favoriteCounts(const QString &rootDir);
void beginGalleryScroll(const QString &rootDir, const QString &session);
void saveGalleryScroll(const QString &rootDir, const QString &session, const QJsonArray &marks);
QJsonValue readGalleryScroll(const QString &rootDir, const QString &session, double y);
std::optional<PageItem> getViewerMedia(PageQuery query, quint64 index);
quint64 createMediaSelection(const PageQuery &query, const QString &token);
void dropMediaSelection(const QString &rootDir, const QString &token);
std::vector<bool> mediaSelectionFlags(const QString &rootDir, const QString &token, const QStringList &paths);
std::vector<PageItem> mediaSelectionBatch(const QString &rootDir, const QString &token, quint64 offset);
void copyMediaSelection(const QString &rootDir, const QString &token, const QStringList &excluded, const QStringList &extra);
int consumeMediaSelection(const QString &rootDir, const QString &token, const QStringList &paths);
int pruneMediaSelection(const QString &rootDir, const QString &token);
std::optional<PageItem> getMediaNeighbor(const PageQuery &query, const QString &currentPath, int delta);
QJsonArray nativeGeometry(const PageQuery &query);
// The in-memory index of an encrypted root; dropped when the root locks.
void releaseMemory(const QString &root);
// A folder below an encrypted root opened as a library of its own: it shows nothing.
bool nestedInVault(const QString &root);

}  // namespace core::catalog
