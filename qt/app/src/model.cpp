#include "model.h"
#include <QTimer>
#include "thumbs.h"
#include <QJsonArray>
MediaItem mediaFromJson(const QJsonObject &o) {
  MediaItem m;
  m.name = o.value(QLatin1String("name")).toString();
  m.revision = o.value(QLatin1String("revision")).toString();
  m.albumPath = o.value(QLatin1String("albumPath")).toString();
  m.albumId = o.value(QLatin1String("albumId")).toString();
  m.albumName = o.value(QLatin1String("albumName")).toString();
  m.thumbName = o.value(QLatin1String("thumbnailName")).toString();
  m.blur = o.value(QLatin1String("blurPreview")).toString();
  m.favorite = o.value(QLatin1String("favorite")).toBool();
  m.isVideo = o.value(QLatin1String("isVideo")).toBool();
  m.width = o.value(QLatin1String("width")).toInt();
  m.height = o.value(QLatin1String("height")).toInt();
  m.dateAdded = o.value(QLatin1String("dateAdded")).toDouble();
  m.dateTaken = o.value(QLatin1String("dateTaken")).toDouble();
  return m;
}
GalleryModel::GalleryModel(CoreClient *core, QObject *parent) : QObject(parent), core_(core) {}
QJsonObject GalleryModel::query(int page) const {
  return {{QStringLiteral("rootDir"), root}, {QStringLiteral("albumId"), album.isEmpty() || favoritesAlbum ? QJsonValue() : QJsonValue(album)}, {QStringLiteral("favoritesOnly"), favoritesOnly || favoritesAlbum}, {QStringLiteral("sortKey"), sortKey}, {QStringLiteral("sortDir"), sortDir}, {QStringLiteral("randomSeed"), randomSeed}, {QStringLiteral("page"), page}};
}
const MediaItem *GalleryModel::item(int index) {
  if (index < 0 || index >= total_) return nullptr;
  auto it = pages_.find(index / kPageSize);
  if (it == pages_.end() || index % kPageSize >= it->size()) return nullptr;
  ages_[index / kPageSize] = ++clock_;
  return &it->at(index % kPageSize);
}
void GalleryModel::require(int first, int last) {
  if (total_ <= 0) return;
  first = qBound(0, first, total_ - 1);
  last = qBound(0, last, total_ - 1);
  for (int page = first / kPageSize; page <= last / kPageSize; page++)
    if (!pages_.contains(page) && !loadingPages_.contains(page)) loadPage(page);
}
void GalleryModel::loadPage(int page) {
  loadingPages_.insert(page);
  loading_++;
  const int version = generation_;
  core_->call(QStringLiteral("get_media_page"), {{QStringLiteral("query"), query(page)}}, [this, page, version](const QJsonValue &result, const QString &error) {
    loading_--;
    if (version != generation_) return;
    loadingPages_.remove(page);
    if (!error.isEmpty()) {
      qWarning("get_media_page: %s", qPrintable(error));
      return;
    }
    const QJsonObject o = result.toObject();
    const QJsonArray rows = o.value(QLatin1String("items")).toArray();
    QVector<MediaItem> items;
    items.reserve(rows.size());
    for (const auto &row : rows) items.push_back(mediaFromJson(row.toObject()));
    const int previous = total_;
    total_ = o.value(QLatin1String("total")).toInt();
    pages_.insert(page, std::move(items));
    ages_[page] = ++clock_;
    while (pages_.size() > kMaxPages) {
      int victim = -1;
      qint64 oldest = std::numeric_limits<qint64>::max();
      for (auto it = ages_.cbegin(); it != ages_.cend(); ++it)
        if (pages_.contains(it.key()) && it.value() < oldest) { oldest = it.value(); victim = it.key(); }
      pages_.remove(victim);
      ages_.remove(victim);
    }
    if (previous != total_) emit reset();
    emit pageLoaded(page * kPageSize, page * kPageSize + rows.size());
  });
}
// A media revision is "<source stamp of 3 fields>-<preview stamp>"; previews appear during processing.
static QStringView sourceRevision(const QString &revision) {
  qsizetype at = -1;
  for (int i = 0; i < 3 && (at = revision.indexOf(QLatin1Char('-'), at + 1)) >= 0; i++) {}
  return at < 0 ? QStringView(revision) : QStringView(revision).left(at);
}
static bool sameInventory(const QVector<MediaItem> &a, const QVector<MediaItem> &b) {
  if (a.size() != b.size()) return false;
  for (int i = 0; i < a.size(); i++)
    if (a[i].name != b[i].name || sourceRevision(a[i].revision) != sourceRevision(b[i].revision)) return false;
  return true;
}
void GalleryModel::captureInventory() {
  inventoryPrint_.clear();
  const int version = generation_;
  core_->call(QStringLiteral("get_inventory_print"), {{QStringLiteral("query"), query()}}, [this, version](const QJsonValue &result, const QString &) {
    if (version == generation_) inventoryPrint_ = result.toString();
  });
}
void GalleryModel::checkInventory(std::function<void(bool)> done) {
  if (inventoryPrint_.isEmpty()) return done(false);
  const QString previous = inventoryPrint_;
  core_->call(QStringLiteral("get_inventory_print"), {{QStringLiteral("query"), query()}}, [previous, done = std::move(done)](const QJsonValue &result, const QString &) { done(result.toString() == previous); });
}
void GalleryModel::reload(std::function<void()> done, bool keepPages, bool inventoryUnchanged) {
  generation_++;
  loading_++;
  const int version = generation_;
  loadingPages_.clear();
  if (!keepPages) pages_.clear(), ages_.clear();
  blurs_.clear();
  // A catalog query without an album reads the whole library; nothing open means nothing to show.
  if (album.isEmpty() && !favoritesAlbum) {
    loading_--;
    total_ = 0;
    pages_.clear();
    ages_.clear();
    emit reset();
    if (done) done();
    return;
  }
  core_->call(QStringLiteral("get_media_page"), {{QStringLiteral("query"), query(0)}}, [this, version, keepPages, inventoryUnchanged, done = std::move(done)](const QJsonValue &result, const QString &error) {
    loading_--;
    if (version != generation_) {
      if (done) done();
      return;
    }
    // Paged requests share a newest-wins slot in the core; a neighbour page must not blank the album.
    if (error.contains(QLatin1String("superseded"))) {
      reload(done, keepPages, inventoryUnchanged);
      return;
    }
    bool keepSelection = false;
    if (error.isEmpty()) {
      const QJsonObject o = result.toObject();
      const QJsonArray rows = o.value(QLatin1String("items")).toArray();
      QVector<MediaItem> items;
      items.reserve(rows.size());
      for (const auto &row : rows) items.push_back(mediaFromJson(row.toObject()));
      const int total = o.value(QLatin1String("total")).toInt();
      keepSelection = inventoryUnchanged && total == total_ && pages_.contains(0) && sameInventory(pages_.value(0), items);
      total_ = total;
      pages_.clear();
      ages_.clear();
      pages_.insert(0, std::move(items));
      ages_[0] = ++clock_;
    } else {
      total_ = 0;
      pages_.clear();
      qWarning("reload: %s", qPrintable(error));
    }
    if (keepSelection) emit pageLoaded(0, kPageSize);
    else emit reset();
    if (done) done();
  });
}
void GalleryModel::refreshInPlace() {
  if (album.isEmpty() && !favoritesAlbum) return;
  const int version = generation_;
  for (const int page : pages_.keys()) {
    if (loadingPages_.contains(page)) continue;
    loadingPages_.insert(page);
    core_->call(QStringLiteral("get_media_page"), {{QStringLiteral("query"), query(page)}}, [this, page, version](const QJsonValue &result, const QString &error) {
      if (version != generation_) return;
      loadingPages_.remove(page);
      if (!error.isEmpty()) return;
      const QJsonObject o = result.toObject();
      const QJsonArray rows = o.value(QLatin1String("items")).toArray();
      QVector<MediaItem> items;
      items.reserve(rows.size());
      for (const auto &row : rows) items.push_back(mediaFromJson(row.toObject()));
      const auto it = pages_.find(page);
      if (o.value(QLatin1String("total")).toInt() != total_ || it == pages_.end() || !sameInventory(*it, items)) {
        reload({}, true);
        return;
      }
      *it = std::move(items);
      emit pageLoaded(page * kPageSize, page * kPageSize + int(it->size()));
    });
  }
  if (geometryVersion_ != version || geometryLoading_) return;
  geometryLoading_ = true;
  core_->call(QStringLiteral("native_geometry"), {{QStringLiteral("query"), query()}}, [this, version](const QJsonValue &result, const QString &error) {
    geometryLoading_ = false;
    if (version != generation_) {
      if (geometryWanted_ || !geometryWaiters_.isEmpty()) ensureGeometry();
      return;
    }
    if (!error.isEmpty()) return;
    const QJsonArray rows = result.toArray();
    if (rows.size() != geometry_.size()) return reload({}, true);
    bool changed = false;
    for (int i = 0; i < rows.size(); i++) {
      const QJsonObject o = rows[i].toObject();
      GeometryItem &g = geometry_[i];
      if (g.name != o.value(QLatin1String("name")).toString()) return reload({}, true);
      const int width = qMax(1, o.value(QLatin1String("width")).toInt()), height = qMax(1, o.value(QLatin1String("height")).toInt());
      const bool favorite = o.value(QLatin1String("favorite")).toBool();
      g.revision = o.value(QLatin1String("revision")).toString();
      if (g.width != width || g.height != height || g.favorite != favorite) {
        g.width = width;
        g.height = height;
        g.favorite = favorite;
        changed = true;
      }
    }
    // Only a changed shape moves tiles; anything else is picked up by the next paint.
    if (changed) emit geometryChanged();
  });
}
void GalleryModel::setRoot(const QString &path) {
  root = path;
  rootGeneration_++;
}
void GalleryModel::refreshAlbums(std::function<void()> done, int retries) {
  const int asked = rootGeneration_, listing = ++listingsStarted_;
  core_->call(QStringLiteral("get_albums_detached"), {{QStringLiteral("rootDir"), root}}, [this, asked, listing, retries, done = std::move(done)](const QJsonValue &result, const QString &error) mutable {
    // An answer about a root that is no longer open must not reach the new one, nor its waiters.
    if (asked != rootGeneration_) return;
    // Nor does a listing that started before the one already shown.
    if (listing < listingAnswered_) {
      if (done) done();
      return;
    }
    if (!error.isEmpty()) {
      qWarning("albums: %s", qPrintable(error));
      // A tree that changes during the walk fails the listing; keep the last list instead of showing an empty library.
      if (retries > 0) {
        QTimer::singleShot(300, this, [this, asked, retries, done = std::move(done)]() mutable {
          if (asked == rootGeneration_) refreshAlbums(std::move(done), retries - 1);
        });
        return;
      }
      if (!albums.isEmpty()) {
        if (done) done();
        return;
      }
    }
    listingAnswered_ = listing;
    albums.clear();
    for (const auto &row : result.toArray()) {
      const QJsonObject o = row.toObject();
      albums.push_back({o.value(QLatin1String("relative_path")).toString(), o.value(QLatin1String("name")).toString(), o.value(QLatin1String("path")).toString(), o.value(QLatin1String("parent")).toString(), o.value(QLatin1String("thumb_path")).toString(), o.value(QLatin1String("directory_id")).toString(), o.value(QLatin1String("size")).toInt()});
    }
    emit albumsChanged();
    if (done) done();
  });
}
QString GalleryModel::pathForAlbumId(const QString &id) const {
  return root + QLatin1Char('/') + id;
}
void GalleryModel::ensureGeometry(std::function<void()> done) {
  if (geometryReady()) {
    if (done) done();
    return;
  }
  // Nothing open: an empty projection, without asking the core about no folder at all.
  if (album.isEmpty() && !favoritesAlbum) {
    geometry_.clear();
    geometryVersion_ = generation_;
    if (done) done();
    return;
  }
  geometryWanted_ = true;
  if (done) geometryWaiters_.push_back(std::move(done));
  if (geometryLoading_) return;
  geometryLoading_ = true;
  const int version = generation_;
  core_->call(QStringLiteral("native_geometry"), {{QStringLiteral("query"), query()}}, [this, version](const QJsonValue &result, const QString &error) {
    geometryLoading_ = false;
    // An answer for an older query: ask again for the current one if anything still needs it.
    if (version != generation_) {
      if (geometryWanted_ || !geometryWaiters_.isEmpty()) ensureGeometry();
      return;
    }
    geometryWanted_ = false;
    if (!error.isEmpty()) qWarning("geometry: %s", qPrintable(error));
    const QJsonArray rows = result.toArray();
    QVector<GeometryItem> next;
    next.reserve(rows.size());
    for (const auto &row : rows) {
      const QJsonObject o = row.toObject();
      next.push_back({o.value(QLatin1String("name")).toString(), o.value(QLatin1String("revision")).toString(), o.value(QLatin1String("albumId")).toString(), o.value(QLatin1String("albumName")).toString(), qMax(1, o.value(QLatin1String("width")).toInt()), qMax(1, o.value(QLatin1String("height")).toInt()), o.value(QLatin1String("favorite")).toBool()});
    }
    geometry_ = std::move(next);
    geometryVersion_ = version;
    emit geometryChanged();
    const auto waiters = std::move(geometryWaiters_);
    geometryWaiters_.clear();
    for (const auto &waiter : waiters) waiter();
  });
}
MediaItem GalleryModel::itemFromGeometry(int index) const {
  MediaItem m;
  if (index < 0 || index >= geometry_.size()) return m;
  const GeometryItem &g = geometry_[index];
  m.name = g.name;
  m.revision = g.revision;
  m.albumId = g.albumId;
  m.albumName = g.albumName;
  m.albumPath = pathForAlbumId(g.albumId);
  m.width = g.width;
  m.height = g.height;
  m.favorite = g.favorite;
  return m;
}
void GalleryModel::refreshFavoriteCount(std::function<void()> done) {
  core_->call(QStringLiteral("favorite_counts"), {{QStringLiteral("rootDir"), root}}, [this, done = std::move(done)](const QJsonValue &result, const QString &) {
    int total = 0;
    const QJsonObject counts = result.toObject();
    for (auto it = counts.begin(); it != counts.end(); ++it) total += it.value().toInt();
    if (result.isDouble()) total = result.toInt();
    if (total != favoriteCount) {
      favoriteCount = total;
      emit albumsChanged();
    }
    if (done) done();
  });
}
const AlbumInfo *GalleryModel::albumById(const QString &id) const {
  for (const auto &a : albums)
    if (a.id == id) return &a;
  return nullptr;
}
const QImage *GalleryModel::blur(const MediaItem &item) {
  if (item.blur.isEmpty()) return nullptr;
  auto it = blurs_.find(item.name);
  if (it == blurs_.end()) {
    if (blurs_.size() > 3000) blurs_.clear();
    it = blurs_.insert(item.name, decodeBlurPreview(item.blur));
  }
  return it->isNull() ? nullptr : &*it;
}
void GalleryModel::loadAll(std::function<void(QVector<MediaItem>)> done) {
  auto out = std::make_shared<QVector<MediaItem>>();
  auto step = std::make_shared<std::function<void(int)>>();
  *step = [this, out, step, done = std::move(done)](int page) {
    core_->call(QStringLiteral("get_media_page"), {{QStringLiteral("query"), query(page)}}, [out, step, done, page](const QJsonValue &result, const QString &) {
      const QJsonObject o = result.toObject();
      const QJsonArray rows = o.value(QLatin1String("items")).toArray();
      for (const auto &row : rows) out->push_back(mediaFromJson(row.toObject()));
      if (rows.isEmpty() || out->size() >= o.value(QLatin1String("total")).toInt()) {
        done(*out);
        QTimer::singleShot(0, [step] { *step = nullptr; });
      } else {
        (*step)(page + 1);
      }
    });
  };
  (*step)(0);
}
