#pragma once
// Paged gallery inventory backed by the core catalog. At most kMaxPages retained.
#include "core_client.h"
#include <QHash>
#include <QImage>
#include <QVector>
struct MediaItem {
  // name is the file on disk; in an encrypted root a random one, with the original in displayName.
  QString name, revision, albumPath, albumId, albumName, thumbName, blur, displayName;
  bool favorite = false, isVideo = false;
  int width = 0, height = 0;
  double dateAdded = 0, dateTaken = 0;
  QString path() const { return albumPath + QLatin1Char('/') + name; }
  QString thumbPath() const { return albumPath + QStringLiteral("/.room237-thumb/") + (thumbName.isEmpty() ? name + QStringLiteral(".webp") : thumbName); }
  double displayDate() const { return dateTaken > 0 ? dateTaken : dateAdded; }
  QString label() const { return displayName.isEmpty() ? name : displayName; }
  QString extension() const;  // Lowercase, of the original name.
};
struct GeometryItem {
  QString name, revision, albumId, albumName;
  int width = 1, height = 1;
  bool favorite = false;
  QString displayName;
};
MediaItem mediaFromJson(const QJsonObject &o);
struct AlbumInfo {
  QString id, name, path, parent, thumbPath, directoryId;
  int size = 0;
};
class GalleryModel : public QObject {
  Q_OBJECT
public:
  static constexpr int kPageSize = 256, kMaxPages = 8;
  explicit GalleryModel(CoreClient *core, QObject *parent = nullptr);
  QString root, album, sortKey = QStringLiteral("name"), sortDir = QStringLiteral("asc");
  bool favoritesOnly = false, favoritesAlbum = false;
  double randomSeed = 0.25;
  int total() const { return total_; }
  QVector<AlbumInfo> albums;
  QJsonObject query(int page = 0) const;
  const MediaItem *item(int index);
  void require(int first, int last);
  // inventoryUnchanged: only derived data moved on, so an identical first page keeps selection and layout.
  void reload(std::function<void()> done = {}, bool keepPages = false, bool inventoryUnchanged = false);
  // While media is being processed: re-reads the loaded pages and, when it is in use, the geometry, replacing them as
  // the answers arrive. What is on screen stays until then; a changed inventory falls back to reload().
  void refreshInPlace();
  // Callbacks of a listing started for an earlier root are dropped.
  void setRoot(const QString &path);
  void refreshAlbums(std::function<void()> done = {}, int retries = 5);
  // Sequence numbers of album listings: the last one started and the one whose answer is shown.
  int listingsStarted() const { return listingsStarted_; }
  int listingAnswered() const { return listingAnswered_; }
  // Loads the complete inventory through ordered page requests.
  void loadAll(std::function<void(QVector<MediaItem>)> done);
  // Selections are index-based: they stay valid only while names and source revisions stay the same.
  void captureInventory();
  void checkInventory(std::function<void(bool same)> done);
  const AlbumInfo *albumById(const QString &id) const;
  QString pathForAlbumId(const QString &id) const;
  // Full ordered projection of the query. Needed by non-uniform layouts, group headers and large selections.
  void ensureGeometry(std::function<void()> done = {});
  bool geometryReady() const { return geometryVersion_ == generation_; }
  const QVector<GeometryItem> &geometry() const { return geometry_; }
  int favoriteCount = 0;
  void refreshFavoriteCount(std::function<void()> done = {});
  MediaItem itemFromGeometry(int index) const;
  bool busy() const { return loading_ != 0; }
  const QImage *blur(const MediaItem &item);
signals:
  void geometryChanged();
  void albumsChanged();
  void reset();
  void pageLoaded(int first, int last);
private:
  void loadPage(int page);
  CoreClient *core_;
  QHash<int, QVector<MediaItem>> pages_;
  QHash<int, qint64> ages_;
  QHash<QString, QImage> blurs_;
  QSet<int> loadingPages_;
  qint64 clock_ = 0;
  int total_ = 0, generation_ = 0, loading_ = 0, rootGeneration_ = 0, listingsStarted_ = 0, listingAnswered_ = 0;
  QString inventoryPrint_;
  QVector<GeometryItem> geometry_;
  int geometryVersion_ = -1;
  bool geometryLoading_ = false, geometryWanted_ = false;
  QVector<std::function<void()>> geometryWaiters_;
};
