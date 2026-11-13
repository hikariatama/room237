#include "catalog.h"

#include "album.h"
#include "album_db.h"
#include "album_tree.h"
#include "clipboard.h"
#include "constants.h"
#include "db.h"
#include "json.h"
#include "media_delete.h"
#include "metadata.h"
#include "preload.h"
#include "settings.h"
#include "thumb.h"
#include "util.h"
#include "vault.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <atomic>
#include <map>
#include <mutex>
#include <cmath>

namespace core::catalog {

namespace {
std::atomic<quint64> pageRequest {0};

qint64 seedOf(double randomSeed) {
  const double scaled = randomSeed * 1000000.0;
  if (std::isnan(scaled)) return 0;
  if (scaled >= 9.2233720368547758e18) return std::numeric_limits<qint64>::max();
  if (scaled <= -9.2233720368547758e18) return std::numeric_limits<qint64>::min();
  return qint64(scaled);
}
db::Value optional(const std::optional<QString> &value) { return value ? db::Value(*value) : db::Value(); }
}  // namespace

bool nestedInVault(const QString &root) { return !lstat(path::join(root, vault::kVaultFile)).isFile && vault::isVaultPath(root); }

namespace {
QString lastComponent(const QString &id) { return id.section(QLatin1Char('/'), -1); }

// An encrypted root keeps its index in memory only: names, dates and blur previews never reach the disk.
std::mutex memoryMutex;
std::map<QString, std::unique_ptr<db::Connection>> memoryKeepers;

QString databaseFor(const QString &root, const QString &dir) {
  if (nestedInVault(root)) {
    // An empty index that is never scanned: the folder shows nothing.
    const QString uri = QStringLiteral("file:room237-nested-%1?mode=memory&cache=shared").arg(sha256Hex(root.toUtf8()).left(16));
    std::lock_guard lock(memoryMutex);
    auto &keeper = memoryKeepers[root];
    if (!keeper) keeper = std::make_unique<db::Connection>(uri);
    return uri;
  }
  if (!lstat(path::join(root, vault::kVaultFile)).isFile) return path::join(dir, QStringLiteral("index.sqlite"));
  const auto keys = vault::keysFor(root);
  if (!keys) fail(QStringLiteral("This library is locked"));
  const QString uri = QStringLiteral("file:room237-vault-%1?mode=memory&cache=shared").arg(keys->id);
  std::lock_guard lock(memoryMutex);
  auto &keeper = memoryKeepers[root];
  if (!keeper) keeper = std::make_unique<db::Connection>(uri);
  return uri;
}

db::Connection open(const QString &root) {
  if (!qaPathAllowed(root)) fail(QStringLiteral("QA fence refused catalog root"));
  // An empty or relative root would put the index into the process's working directory.
  if (!path::isAbsolute(root) || !isDir(root)) fail(QStringLiteral("Library root is not a directory: %1").arg(root));
  const QString dir = path::join(root, kDirectory);
  if (!exists(dir) && !nestedInVault(root)) createDir(dir);
  db::Connection conn(databaseFor(root, dir));
  conn.busyTimeout(30000);
  conn.executeBatch(QStringLiteral(
      "PRAGMA cache_size=-2048; PRAGMA temp_store=FILE; PRAGMA temp.cache_size=-2048; PRAGMA mmap_size=0;"
      "CREATE TABLE IF NOT EXISTS media (album TEXT NOT NULL, name TEXT NOT NULL, meta TEXT NOT NULL, favorite INTEGER NOT NULL, added INTEGER NOT NULL, shoot INTEGER NOT NULL, blur_preview TEXT, seen INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(album,name));"
      "CREATE INDEX IF NOT EXISTS media_name ON media(album,name COLLATE NOCASE,name);"
      "CREATE INDEX IF NOT EXISTS media_added ON media(album,added,name);"
      "CREATE INDEX IF NOT EXISTS media_shoot ON media(album,shoot,name);"
      "CREATE INDEX IF NOT EXISTS media_favorites ON media(favorite,album,name);"
      "CREATE TABLE IF NOT EXISTS scanned (album TEXT PRIMARY KEY, generation INTEGER NOT NULL,signature TEXT NOT NULL DEFAULT '');"));
  auto hasColumn = [&](const char *sql) {
    try {
      conn.prepare(QString::fromLatin1(sql));
      return true;
    } catch (const Error &) {
      return false;
    }
  };
  if (!hasColumn("SELECT signature FROM scanned LIMIT 0")) conn.execute(QStringLiteral("ALTER TABLE scanned ADD COLUMN signature TEXT NOT NULL DEFAULT ''"));
  if (!hasColumn("SELECT label FROM media LIMIT 0")) {
    try {
      conn.execute(QStringLiteral("ALTER TABLE media ADD COLUMN label TEXT"));
    } catch (const Error &) {
      if (!hasColumn("SELECT label FROM media LIMIT 0")) throw;
    }
  }
  if (!hasColumn("SELECT blur_preview FROM media LIMIT 0")) {
    try {
      conn.execute(QStringLiteral("ALTER TABLE media ADD COLUMN blur_preview TEXT"));
    } catch (const Error &) {
      // Another indexing worker may have completed the migration first.
      if (!hasColumn("SELECT blur_preview FROM media LIMIT 0")) throw;
    }
  }
  conn.scalarFunction("album_label", 1, [](const std::vector<db::Value> &args) -> db::Value {
    const QString *album = std::get_if<QString>(&args[0]);
    if (!album) fail(QStringLiteral("Invalid column type"));
    return lastComponent(*album).toLower();
  });
  conn.scalarFunction("shuffle", 2, [](const std::vector<db::Value> &args) -> db::Value {
    const QString *name = std::get_if<QString>(&args[0]);
    const qint64 *seed = std::get_if<qint64>(&args[1]);
    if (!name || !seed) fail(QStringLiteral("Invalid column type"));
    quint64 hash = quint64(*seed) ^ 0xcbf29ce484222325ULL;
    for (char byte : name->toUtf8()) hash = (hash ^ quint64(quint8(byte))) * 0x100000001b3ULL;
    return qint64(hash);
  });
  return conn;
}

QString pendingMarker(const QString &root, const QString &id) { return path::join(path::join(path::join(root, kDirectory), QStringLiteral("pending")), sha256Hex(id.toUtf8())); }

void put(db::Connection &conn, const QString &album, const QString &name, const QString &path, const FileMetaEntry &entry, std::optional<qint64> seen) {
  QString albumDir = path::parent(path);
  if (albumDir.isEmpty()) albumDir = path;
  if (!constants::isMedia(path) || (path::hasExtension(path, {"heic"}) && metadata::heicHiddenByEntry(albumDir, entry))) return;
  const FileInfo info = stat(path);
  if (!info.isFile) {
    conn.execute(QStringLiteral("DELETE FROM media WHERE album=?1 AND name=?2"), {album, name});
    return;
  }
  DetachedFileMeta meta;
  bool fromEntry = false;
  if (entry.meta && entry.metaSourceRevision && entry.metaSourceRevision == metadata::sourceRevisionFromInfo(info) && validPackedMeta(*entry.meta)) {
    meta = unpackFileMeta(*entry.meta);
    fromEntry = true;
  }
  if (!fromEntry) {
    if (info.birthSec && *info.birthSec >= 0) meta.a = quint64(*info.birthSec);
    meta.s = metadata::readEmbeddedTimestamp(path);
    meta.i = constants::isImage(path);
    meta.v = constants::isVideo(path);
  }
  const qint64 added = meta.a ? qint64(*meta.a) : meta.s.value_or(0);
  const qint64 shoot = meta.s ? *meta.s : (meta.a ? qint64(*meta.a) : 0);
  conn.execute(QStringLiteral("INSERT INTO media(album,name,meta,favorite,added,shoot,seen,blur_preview,label) VALUES(?1,?2,?3,?4,?5,?6,coalesce(?7,0),?8,?9) "
                              "ON CONFLICT(album,name) DO UPDATE SET meta=excluded.meta,favorite=excluded.favorite,added=excluded.added,shoot=excluded.shoot,seen=coalesce(?7,media.seen),blur_preview=excluded.blur_preview,label=excluded.label "
                              "WHERE media.blur_preview IS NOT excluded.blur_preview OR media.meta<>excluded.meta OR media.favorite<>excluded.favorite OR media.added<>excluded.added OR media.shoot<>excluded.shoot OR media.label IS NOT excluded.label OR (?7 IS NOT NULL AND media.seen<>excluded.seen)"),
               {album, name, meta.pack(), qint64(entry.favorite), added, shoot, seen ? db::Value(*seen) : db::Value(), optional(entry.blurPreview), optional(entry.vaultName)});
}

// Metadata is authoritative. A failed derived-index write is retried from disk before the next page or favorite-count
// request, including after a restart.
void repairPending(const QString &root) {
  const QString pending = path::join(path::join(root, kDirectory), QStringLiteral("pending"));
  if (!isDir(pending)) return;
  for (const DirEntry &file : readDir(pending)) {
    const QString marker = path::join(pending, file.name);
    QString id;
    try {
      id = QString::fromUtf8(readAll(marker));
    } catch (const Error &) {
      if (!entryExists(marker)) continue;
      throw;
    }
    const QString album = path::join(root, id);
    if (isDir(album)) {
      if (!path::startsWith(canonical(album), canonical(root))) fail(QStringLiteral("Pending album outside library"));
      scanAlbum(root, album);
    } else {
      db::Connection conn = open(root);
      conn.execute(QStringLiteral("DELETE FROM media WHERE album=?1"), {id});
      removeFileIfExists(marker);
    }
  }
}

PageItem itemFor(const QString &root, const QString &album, const QString &name, const QString &meta, bool favorite, const std::optional<QString> &blur, const QString &albumName, const std::optional<QString> &label = std::nullopt) {
  const QString dir = path::join(root, album);
  const QString file = path::join(dir, name);
  PageItem item;
  item.media.blurPreview = blur;
  item.media.revision = metadata::mediaRevision(file);
  item.media.thumbnailName = thumb::detachedThumbnailName(file);
  item.media.name = name;
  item.media.meta = meta;
  item.media.favorite = favorite;
  item.albumPath = dir;
  item.albumId = album;
  item.albumName = albumName;
  item.media.displayName = label;
  return item;
}

QString orderKey(const QString &sortKey, int seedIndex) {
  if (sortKey == QLatin1String("added")) return QStringLiteral("added");
  if (sortKey == QLatin1String("shoot")) return QStringLiteral("shoot");
  if (sortKey == QLatin1String("random")) return QStringLiteral("shuffle(album || '/' || name, ?%1)").arg(seedIndex);
  return QStringLiteral("coalesce(label,name) COLLATE NOCASE");
}

MediaPage queryPageInner(const PageQuery &query, std::optional<quint64> request) {
  db::Connection conn = open(query.rootDir);
  if (request) {
    if (pageRequest.load(std::memory_order_relaxed) != *request) fail(QStringLiteral("Page request superseded"));
    const quint64 id = *request;
    conn.progressHandler(1000, [id] { return pageRequest.load(std::memory_order_relaxed) != id; });
  }
  conn.executeBatch(QStringLiteral("BEGIN DEFERRED"));
  const QString direction = query.sortDir == QLatin1String("desc") ? QStringLiteral("DESC") : QStringLiteral("ASC");
  const QString order = orderKey(query.sortKey, 3);
  const QString filter = query.albumId ? QStringLiteral("album=?1 AND (?2=0 OR favorite=1)") : QStringLiteral("?1 IS NULL AND (?2=0 OR favorite=1)");
  quint64 total = 0;
  {
    db::Statement count = conn.prepare(QStringLiteral("SELECT count(*) FROM media WHERE %1").arg(filter));
    count.bind(1, optional(query.albumId)).bind(2, qint64(query.favoritesOnly));
    if (!count.step()) fail(QStringLiteral("Query returned no rows"));
    total = quint64(count.integer(0));
  }
  const quint64 page = std::min(query.page, (total ? total - 1 : 0) / kPageSize);
  const int limit = kPageSize * int(std::clamp<quint64>(query.limitPages.value_or(1), 1, 5));
  const QString group = query.albumId ? QString() : QStringLiteral("album_label(album) ASC, album ASC, ");
  struct Row {
    QString album, name, meta;
    bool favorite;
    std::optional<QString> blur, label;
  };
  std::vector<Row> rows;
  {
    db::Statement stmt = conn.prepare(QStringLiteral("SELECT album,name,meta,favorite,blur_preview,label FROM media WHERE %1 ORDER BY %2%3 %4, album %4, name %4 LIMIT ?4 OFFSET ?5").arg(filter, group, order, direction));
    stmt.bind(1, optional(query.albumId)).bind(2, qint64(query.favoritesOnly)).bind(3, seedOf(query.randomSeed)).bind(4, qint64(limit)).bind(5, qint64(page * kPageSize));
    while (stmt.step()) rows.push_back({stmt.text(0), stmt.text(1), stmt.text(2), stmt.integer(3) != 0, stmt.optionalText(4), stmt.optionalText(5)});
  }
  conn.executeBatch(QStringLiteral("COMMIT"));
  conn.close();
  MediaPage result;
  result.total = total;
  result.page = page;
  for (const Row &row : rows) result.items.push_back(itemFor(query.rootDir, row.album, row.name, row.meta, row.favorite, row.blur, lastComponent(row.album), row.label));
  return result;
}

db::Connection scrollStore(const QString &root) {
  db::Connection conn = open(root);
  conn.executeBatch(QStringLiteral("CREATE TABLE IF NOT EXISTS scroll_marks(session TEXT NOT NULL,page INTEGER NOT NULL,y REAL NOT NULL,data TEXT NOT NULL,PRIMARY KEY(session,page)); CREATE INDEX IF NOT EXISTS scroll_marks_y ON scroll_marks(session,y);"));
  return conn;
}
db::Connection selectionStore(const QString &root) {
  db::Connection conn = open(root);
  conn.executeBatch(QStringLiteral("CREATE TABLE IF NOT EXISTS selection_rows(token TEXT NOT NULL,idx INTEGER NOT NULL,album TEXT NOT NULL,name TEXT NOT NULL,meta TEXT NOT NULL,favorite INTEGER NOT NULL,PRIMARY KEY(token,idx)); CREATE INDEX IF NOT EXISTS selection_names ON selection_rows(token,album,name);"));
  return conn;
}

QStringList albumIds(const QString &root, const QStringList &albums) {
  QStringList ids;
  for (const QString &album : albums) ids << path::relative(album, root).replace(QLatin1Char('\\'), QLatin1Char('/'));
  return ids;
}
}  // namespace

PageQuery PageQuery::fromJson(const QJsonValue &value) {
  if (!value.isObject()) fail(QStringLiteral("query: invalid type, expected struct PageQuery"));
  const QJsonObject o = value.toObject();
  PageQuery q;
  q.rootDir = json::stringField(o.value(QLatin1String("rootDir")), "rootDir");
  const QJsonValue album = o.value(QLatin1String("albumId"));
  if (!album.isNull() && !album.isUndefined()) q.albumId = json::stringField(album, "albumId");
  q.favoritesOnly = json::boolField(o.value(QLatin1String("favoritesOnly")), "favoritesOnly");
  q.sortKey = json::stringField(o.value(QLatin1String("sortKey")), "sortKey");
  q.sortDir = json::stringField(o.value(QLatin1String("sortDir")), "sortDir");
  q.randomSeed = json::floatField(o.value(QLatin1String("randomSeed")), "randomSeed");
  q.page = json::unsignedField(o.value(QLatin1String("page")), "page", ~0ull);
  const QJsonValue limit = o.value(QLatin1String("limitPages"));
  if (!limit.isNull() && !limit.isUndefined()) q.limitPages = json::unsignedField(limit, "limitPages", ~0ull);
  return q;
}

QJsonObject PageItem::toJson() const {
  QJsonObject o = media.toJson();
  o.insert(QStringLiteral("albumPath"), albumPath);
  o.insert(QStringLiteral("albumId"), albumId);
  o.insert(QStringLiteral("albumName"), albumName);
  return o;
}
QJsonObject MediaPage::toJson() const {
  QJsonArray array;
  for (const PageItem &item : items) array.append(item.toJson());
  return {{QStringLiteral("items"), array}, {QStringLiteral("total"), qint64(total)}, {QStringLiteral("page"), qint64(page)}, {QStringLiteral("pageSize"), kPageSize}};
}

QString albumId(const QString &root, const QString &album) {
  if (!path::startsWith(album, root)) fail(QStringLiteral("Album outside library"));
  return path::relative(album, root).replace(QLatin1Char('\\'), QLatin1Char('/'));
}

std::optional<QString> libraryRoot(const QString &album) {
  for (const QString &ancestor : path::ancestors(album))
    if (isDir(path::join(ancestor, kDirectory))) return ancestor;
  return std::nullopt;
}

void publish(const QString &album, const QString &name, const FileMetaEntry &entry) {
  const auto root = libraryRoot(album);
  if (!root) return;
  try {
    db::Connection conn = open(*root);
    put(conn, albumId(*root, album), name, path::join(album, name), entry, std::nullopt);
  } catch (const Error &error) {
    qWarning("Catalog update failed for %s: %s", qUtf8Printable(album), qUtf8Printable(error.message()));
    try {
      const QString id = albumId(*root, album);
      const QString marker = pendingMarker(*root, id);
      createDirs(path::parent(marker));
      writeAll(marker, id.toUtf8());
    } catch (const Error &retry) {
      qWarning("Cannot record pending catalog repair: %s", qUtf8Printable(retry.message()));
    }
  }
}

void forget(const QString &album, const QStringList &names) {
  const auto root = libraryRoot(album);
  if (!root) return;
  db::Connection conn = open(*root);
  db::Transaction tx(conn);
  const QString id = albumId(*root, album);
  for (const QString &name : names) conn.execute(QStringLiteral("DELETE FROM media WHERE album=?1 AND name=?2"), {id, name});
  tx.commit();
}

bool reconcileAlbum(const QString &rootDir, const QString &id) {
  if (nestedInVault(rootDir)) return false;
  const QString root = rootDir;
  initialize(root);
  const QString album = path::join(root, id);
  if (!isDir(album)) {
    retainAlbums(root, albumIds(root, album::walkAlbumPaths(root)));
    return true;
  }
  if (!path::startsWith(canonical(album), canonical(root))) fail(QStringLiteral("Album outside library"));
  QSet<QString> known;
  {
    db::Connection conn = open(root);
    db::Statement statement = conn.prepare(QStringLiteral("SELECT name FROM media WHERE album=?1"));
    statement.bind(1, id);
    while (statement.step()) known.insert(statement.text(0));
  }
  const QSet<QString> hidden = metadata::hiddenHeicNames(album);
  const QString database = path::join(album, QStringLiteral(".room237-metadata/album.sqlite"));
  std::optional<album_db::FileReader> reader;
  if (isFile(database)) reader.emplace(database);
  int seen = 0;
  bool changed = false;
  QSet<QString> children;
  for (const DirEntry &entry : readDir(album)) {
    const QString path = path::join(album, entry.name);
    if (album::isAlbumDir(path)) {
      children.insert(id + QLatin1Char('/') + entry.name);
      continue;
    }
    if (!entry.isFile || !constants::isMedia(path)) continue;
    if (hidden.contains(entry.name)) continue;
    ++seen;
    if (!known.contains(entry.name)) {
      changed = true;
      break;
    }
    // An entry without a stored revision is still being processed, which is not a change.
    std::optional<QString> stored;
    if (reader)
      if (auto row = reader->get(entry.name)) stored = row->metaSourceRevision;
    if (stored && stored != metadata::sourceRevision(path)) {
      changed = true;
      break;
    }
  }
  if (!changed && seen == known.size()) {
    // A nested album renamed or moved here changes only this listing; the caller refreshes the tree.
    db::Connection conn = open(root);
    const QString prefix = id + QLatin1Char('/');
    db::Statement statement = conn.prepare(QStringLiteral("SELECT album FROM scanned"));
    QSet<QString> scanned;
    while (statement.step()) {
      const QString other = statement.text(0);
      if (other.startsWith(prefix) && !other.mid(prefix.size()).contains(QLatin1Char('/'))) scanned.insert(other);
    }
    return scanned != children;
  }
  scanAlbum(root, album);
  preload::dropPreloadForPath(album);
  preload::enqueuePreload(album);
  preload::startPreloaderWorker();
  return true;
}

int scanAlbum(const QString &root, const QString &album) { return scanAlbumIfNeeded(root, album, true); }

int scanAlbumIfNeeded(const QString &root, const QString &album, bool force) {
  if (nestedInVault(root)) return 0;
  auto tree = album_tree::read({album});
  auto inventory = FileLock::acquire(path::join(album, QStringLiteral(".room237-inventory.lock")), std::chrono::seconds(1800), std::chrono::milliseconds(20));
  media_delete::recoverAlbum(album);
  const QString id = albumId(root, album);
  auto signature = [&] { return metadata::sourceRevision(album).value_or(QString()) + QLatin1Char('|') + readSettings().processingVersion(); };
  if (!force && !isFile(pendingMarker(root, id))) {
    db::Connection conn = open(root);
    db::Statement old = conn.prepare(QStringLiteral("SELECT signature FROM scanned WHERE album=?1"));
    old.bind(1, id);
    if (old.step() && old.text(0) == signature()) {
      db::Statement count = conn.prepare(QStringLiteral("SELECT count(*) FROM media WHERE album=?1"));
      count.bind(1, id);
      count.step();
      return int(count.integer(0));
    }
  }
  metadata::prepareDatabase(album);
  album_db::FileReader reader(path::join(album, QStringLiteral(".room237-metadata/album.sqlite")));
  db::Connection conn = open(root);
  const qint64 generation = qint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  int count = 0;
  QStringList batch;
  auto flush = [&] {
    auto guard = metadata::lockMetadataForWrite({album});
    db::Transaction tx(conn);
    for (const QString &name : batch) put(conn, id, name, path::join(album, name), reader.get(name).value_or(FileMetaEntry()), generation);
    tx.commit();
    batch.clear();
  };
  const QSet<QString> hidden = metadata::hiddenHeicNames(album);
  for (const DirEntry &entry : readDir(album)) {
    const QString path = path::join(album, entry.name);
    if (!entry.isFile || !constants::isMedia(path) || hidden.contains(entry.name)) continue;
    batch << entry.name;
    ++count;
    if (batch.size() == kPageSize) flush();
  }
  flush();
  {
    auto guard = metadata::lockMetadataForWrite({album});
    db::Transaction tx(conn);
    conn.execute(QStringLiteral("DELETE FROM media WHERE album=?1 AND seen<>?2"), {id, generation});
    conn.execute(QStringLiteral("INSERT INTO scanned(album,generation,signature) VALUES(?1,?2,?3) ON CONFLICT(album) DO UPDATE SET generation=excluded.generation,signature=excluded.signature"), {id, generation, signature()});
    tx.commit();
  }
  removeFileIfExists(pendingMarker(root, id));
  return count;
}

void initialize(const QString &root) { open(root); }

void retainAlbums(const QString &root, const QStringList &ids) {
  db::Connection conn = open(root);
  db::Transaction tx(conn);
  conn.executeBatch(QStringLiteral("CREATE TEMP TABLE live_albums(id TEXT PRIMARY KEY)"));
  for (const QString &id : ids) conn.execute(QStringLiteral("INSERT INTO live_albums VALUES(?1)"), {id});
  conn.execute(QStringLiteral("DELETE FROM media WHERE album NOT IN(SELECT id FROM live_albums)"));
  conn.execute(QStringLiteral("DELETE FROM scanned WHERE album NOT IN(SELECT id FROM live_albums)"));
  tx.commit();
}

void clear(const QString &root) { open(root).executeBatch(QStringLiteral("DELETE FROM media; DELETE FROM scanned;")); }

void rescanInventory(const QString &root, const std::optional<QString> &id) {
  if (nestedInVault(root)) return;
  initialize(root);
  if (id) {
    const QString album = path::join(root, *id);
    if (!path::startsWith(canonical(album), canonical(root))) fail(QStringLiteral("Album outside library"));
    scanAlbum(root, album);
    preload::dropPreloadForPath(album);
    preload::enqueuePreload(album);
  } else {
    const QStringList albums = album::walkAlbumPaths(root);
    for (const QString &album : albums) {
      scanAlbum(root, album);
      preload::dropPreloadForPath(album);
      preload::enqueuePreload(album);
    }
    retainAlbums(root, albumIds(root, albums));
  }
  preload::startPreloaderWorker();
}

void refreshCatalogPaths(const QString &rootDir, const QStringList &paths) {
  if (nestedInVault(rootDir)) return;
  if (paths.size() > kPageSize) fail(QStringLiteral("Too many changed paths in one batch"));
  const QString root = canonical(rootDir);
  for (const QString &changed : paths) {
    const QString parent = path::parent(changed);
    if (parent.isEmpty()) fail(QStringLiteral("Missing album"));
    const QString album = canonical(parent);
    if (!path::startsWith(album, root)) fail(QStringLiteral("Album outside library"));
    const QString name = path::fileName(changed);
    if (name.isEmpty()) fail(QStringLiteral("Invalid media name"));
    const QString file = path::join(album, name);
    if (isFile(file)) publish(album, name, metadata::readFileEntry(file).value_or(FileMetaEntry()));
    else forget(album, {name});
  }
}

MediaPage queryPage(const PageQuery &query) { return queryPageInner(query, std::nullopt); }

QString getInventoryPrint(PageQuery query) {
  QCryptographicHash hasher(QCryptographicHash::Sha256);
  query.page = 0;
  for (;;) {
    const MediaPage page = queryPage(query);
    for (const PageItem &item : page.items) {
      hasher.addData(item.media.name.toUtf8());
      hasher.addData(metadata::sourceRevision(path::join(item.albumPath, item.media.name)).value_or(QString()).toUtf8());
    }
    const quint64 seen = (query.page + 1) * kPageSize;
    if (page.items.empty() || seen >= page.total) return toHex(hasher.result());
    ++query.page;
  }
}

MediaPage getMediaPage(const PageQuery &query) {
  const quint64 request = pageRequest.fetch_add(1) + 1;
  repairPending(query.rootDir);
  MediaPage page = queryPageInner(query, request);
  const AdvancedSettings settings = readSettings();
  const QString version = settings.thumbVersion();
  std::optional<std::pair<QString, std::unique_ptr<album_db::FileReader>>> reader;
  for (const PageItem &item : page.items) {
    if (pageRequest.load(std::memory_order_relaxed) != request) fail(QStringLiteral("Page request superseded"));
    const QString file = path::join(item.albumPath, item.media.name);
    if (!isFile(file)) continue;
    if (!reader || reader->first != item.albumPath) reader.emplace(item.albumPath, std::make_unique<album_db::FileReader>(path::join(item.albumPath, QStringLiteral(".room237-metadata/album.sqlite"))));
    const FileMetaEntry entry = reader->second->get(item.media.name).value_or(FileMetaEntry());
    const QString thumbs = path::join(item.albumPath, thumb::kThumbDir);
    if (!thumb::cachedThumbCurrent(file, thumbs, entry, version)) preload::enqueueThumbTask(file, thumbs, false, preload::TaskPriority::High);
    if (!metadata::metadataCacheCurrentWithSettings(file, entry, settings)) preload::enqueueMetaTask(file, false, preload::TaskPriority::High);
  }
  return page;
}

QJsonObject favoriteCounts(const QString &rootDir) {
  repairPending(rootDir);
  db::Connection conn = open(rootDir);
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT album,count(*) FROM media WHERE favorite=1 GROUP BY album"));
  QJsonObject counts;
  while (stmt.step()) counts.insert(stmt.text(0), stmt.integer(1));
  return counts;
}

void beginGalleryScroll(const QString &rootDir, const QString &session) { scrollStore(rootDir).execute(QStringLiteral("DELETE FROM scroll_marks WHERE session<>?1"), {session}); }

void saveGalleryScroll(const QString &rootDir, const QString &session, const QJsonArray &marks) {
  if (marks.size() > 6) fail(QStringLiteral("Too many scroll checkpoints"));
  db::Connection conn = scrollStore(rootDir);
  db::Transaction tx(conn);
  for (const QJsonValue &value : marks) {
    const QJsonObject mark = value.toObject();
    const quint64 page = json::unsignedField(mark.value(QLatin1String("page")), "page", ~0ull);
    const double y = json::floatField(mark.value(QLatin1String("y")), "y");
    const QJsonValue cursor = mark.value(QLatin1String("cursor"));
    QString cursorText = QString::fromUtf8(QJsonDocument(QJsonArray{cursor.isUndefined() ? QJsonValue() : cursor}).toJson(QJsonDocument::Compact));
    cursorText = cursorText.mid(1, cursorText.size() - 2);
    if (!std::isfinite(y) || y < 0.0 || cursorText.size() > 65536) fail(QStringLiteral("Invalid scroll checkpoint"));
    if (page > quint64(std::numeric_limits<qint64>::max())) fail(QStringLiteral("out of range integral type conversion attempted"));
    conn.execute(QStringLiteral("INSERT INTO scroll_marks VALUES(?1,?2,?3,?4) ON CONFLICT(session,page) DO UPDATE SET y=excluded.y,data=excluded.data"), {session, qint64(page), y, cursorText});
  }
  tx.commit();
}

QJsonValue readGalleryScroll(const QString &rootDir, const QString &session, double y) {
  db::Connection conn = scrollStore(rootDir);
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT page,y,data FROM scroll_marks WHERE session=?1 AND y<=?2 ORDER BY y DESC LIMIT 1"));
  stmt.bind(1, session).bind(2, db::Value(std::max(y, 0.0)));
  if (!stmt.step()) return QJsonValue();
  QJsonParseError error;
  const QByteArray data = stmt.text(2).toUtf8();
  QJsonValue cursor;
  const QJsonDocument document = QJsonDocument::fromJson("[" + data + "]", &error);
  if (error.error != QJsonParseError::NoError) fail(error.errorString());
  cursor = document.array().first();
  return QJsonObject{{QStringLiteral("page"), stmt.integer(0)}, {QStringLiteral("y"), stmt.real(1)}, {QStringLiteral("cursor"), cursor}};
}

std::optional<PageItem> getViewerMedia(PageQuery query, quint64 index) {
  query.page = index / kPageSize;
  MediaPage page = queryPage(query);
  index = std::min(index, page.total ? page.total - 1 : 0);
  const size_t at = size_t(index % kPageSize);
  if (at >= page.items.size()) return std::nullopt;
  return page.items[at];
}

quint64 createMediaSelection(const PageQuery &query, const QString &token) {
  db::Connection conn = selectionStore(query.rootDir);
  const QString direction = query.sortDir == QLatin1String("desc") ? QStringLiteral("DESC") : QStringLiteral("ASC");
  const QString key = orderKey(query.sortKey, 4);
  const QString group = query.albumId ? QString() : QStringLiteral("album_label(album) ASC, album ASC, ");
  const QString order = QStringLiteral("%1%2 %3,album %3,name %3").arg(group, key, direction);
  db::Transaction tx(conn);
  conn.execute(QStringLiteral("DELETE FROM selection_rows WHERE token=?1"), {token});
  conn.execute(QStringLiteral("INSERT INTO selection_rows SELECT ?1,row_number() OVER(ORDER BY %1),album,name,meta,favorite FROM media WHERE (?2 IS NULL OR album=?2) AND (?3=0 OR favorite=1) AND (?4 IS NOT NULL)").arg(order),
               {token, optional(query.albumId), qint64(query.favoritesOnly), seedOf(query.randomSeed)});
  db::Statement count = conn.prepare(QStringLiteral("SELECT count(*) FROM selection_rows WHERE token=?1"));
  count.bind(1, token);
  count.step();
  const quint64 total = quint64(count.integer(0));
  tx.commit();
  return total;
}

void dropMediaSelection(const QString &rootDir, const QString &token) { selectionStore(rootDir).execute(QStringLiteral("DELETE FROM selection_rows WHERE token=?1"), {token}); }

std::vector<bool> mediaSelectionFlags(const QString &rootDir, const QString &token, const QStringList &paths) {
  if (paths.size() > kPageSize * 5 + 12) fail(QStringLiteral("Too many selection paths"));
  db::Connection conn = selectionStore(rootDir);
  std::vector<bool> flags;
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT EXISTS(SELECT 1 FROM selection_rows WHERE token=?1 AND album=?2 AND name=?3)"));
  for (const QString &file : paths) {
    const QString album = path::parent(file);
    if (album.isEmpty()) fail(QStringLiteral("Missing album"));
    const QString name = path::fileName(file);
    if (name.isEmpty()) fail(QStringLiteral("Missing media name"));
    stmt.reset();
    stmt.bind(1, token).bind(2, albumId(rootDir, album)).bind(3, name);
    stmt.step();
    flags.push_back(stmt.integer(0) != 0);
  }
  return flags;
}

std::vector<PageItem> mediaSelectionBatch(const QString &rootDir, const QString &token, quint64 offset) {
  db::Connection conn = selectionStore(rootDir);
  if (offset > quint64(std::numeric_limits<qint64>::max())) fail(QStringLiteral("out of range integral type conversion attempted"));
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT s.album,s.name,s.meta,s.favorite,m.blur_preview,m.label FROM selection_rows s LEFT JOIN media m ON m.album=s.album AND m.name=s.name WHERE s.token=?1 AND s.idx>?2 ORDER BY s.idx LIMIT 256"));
  stmt.bind(1, token).bind(2, qint64(offset));
  std::vector<PageItem> items;
  while (stmt.step()) {
    const QString album = stmt.text(0);
    items.push_back(itemFor(rootDir, album, stmt.text(1), stmt.text(2), stmt.integer(3) != 0, stmt.optionalText(4), album, stmt.optionalText(5)));
  }
  return items;
}

void copyMediaSelection(const QString &rootDir, const QString &token, const QStringList &excluded, const QStringList &extra) {
  db::Connection conn = selectionStore(rootDir);
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT album,name FROM selection_rows WHERE token=?1 ORDER BY idx"));
  stmt.bind(1, token);
  const QSet<QString> skip(excluded.begin(), excluded.end());
  // Clipboard APIs require their output file list. Copy only paths, without loading album records or images.
  QStringList paths;
  while (stmt.step()) {
    const QString file = path::join(path::join(rootDir, stmt.text(0)), stmt.text(1));
    if (!skip.contains(file)) paths << file;
  }
  paths << extra;
  clipboard::setClipboardFiles(paths);
}

int consumeMediaSelection(const QString &rootDir, const QString &token, const QStringList &paths) {
  if (paths.size() > kPageSize) fail(QStringLiteral("Too many selection paths"));
  db::Connection conn = selectionStore(rootDir);
  db::Transaction tx(conn);
  int removed = 0;
  for (const QString &file : paths) {
    const QString album = path::parent(file);
    if (album.isEmpty()) fail(QStringLiteral("Missing album"));
    const QString name = path::fileName(file);
    if (name.isEmpty()) fail(QStringLiteral("Missing media name"));
    removed += conn.execute(QStringLiteral("DELETE FROM selection_rows WHERE token=?1 AND album=?2 AND name=?3"), {token, albumId(rootDir, album), name});
  }
  tx.commit();
  return removed;
}

int pruneMediaSelection(const QString &rootDir, const QString &token) {
  repairPending(rootDir);
  return selectionStore(rootDir).execute(QStringLiteral("DELETE FROM selection_rows WHERE token=?1 AND NOT EXISTS(SELECT 1 FROM media WHERE media.album=selection_rows.album AND media.name=selection_rows.name)"), {token});
}

std::optional<PageItem> getMediaNeighbor(const PageQuery &query, const QString &currentPath, int delta) {
  const QString root = query.rootDir;
  const QString dir = path::parent(currentPath);
  if (dir.isEmpty()) fail(QStringLiteral("Missing album"));
  const QString currentAlbum = albumId(root, dir);
  const QString currentName = path::fileName(currentPath);
  if (currentName.isEmpty()) fail(QStringLiteral("Missing file name"));
  db::Connection conn = open(root);
  const qint64 seed = seedOf(query.randomSeed);
  const QString key = orderKey(query.sortKey, 1);
  db::Value value;
  {
    db::Statement stmt = conn.prepare(QStringLiteral("SELECT %1 FROM media WHERE (?1 IS NOT NULL) AND album=?2 AND name=?3").arg(key));
    stmt.bind(1, seed).bind(2, currentAlbum).bind(3, currentName);
    if (!stmt.step()) return std::nullopt;
    value = stmt.value(0);
  }
  const bool forward = delta >= 0;
  const bool ascending = (query.sortDir != QLatin1String("desc")) == forward;
  const QString comparison = ascending ? QStringLiteral(">") : QStringLiteral("<");
  const QString direction = ascending ? QStringLiteral("ASC") : QStringLiteral("DESC");
  const QString groupComparison = forward ? QStringLiteral(">") : QStringLiteral("<");
  const QString groupDirection = forward ? QStringLiteral("ASC") : QStringLiteral("DESC");
  QString after = QStringLiteral("(%1%2?4 OR (%1=?4 AND (album%2?5 OR (album=?5 AND name%2?6))))").arg(key, comparison);
  if (!query.albumId) after = QStringLiteral("(album_label(album)%1?7 OR (album_label(album)=?7 AND (album%1?5 OR (album=?5 AND %2))))").arg(groupComparison, after);
  const QString grouping = query.albumId ? QString() : QStringLiteral("album_label(album) %1,album %1, ").arg(groupDirection);
  const QString label = lastComponent(currentAlbum).toLower();
  const QString localAfter = QStringLiteral("(%1,name)%2(?4,?6)").arg(key, comparison);
  const QString any = QStringLiteral("(?4 IS NOT NULL AND ?5 IS NOT NULL AND ?6 IS NOT NULL)");
  std::vector<std::pair<bool, QString>> attempts;
  if (query.albumId) attempts = {{true, localAfter}, {true, any}};
  else attempts = {{true, localAfter}, {false, after}, {false, any}};
  for (const auto &[local, condition] : attempts) {
    const QString filter = local ? QStringLiteral("album=?2") : QStringLiteral("?2 IS NULL");
    const QString group = local ? QString() : grouping;
    db::Statement stmt = conn.prepare(QStringLiteral("SELECT album,name,meta,favorite,blur_preview,label FROM media WHERE (?1 IS NOT NULL) AND %1 AND (?3=0 OR favorite=1) AND (?7 IS NOT NULL) AND %2 ORDER BY %3%4 %5,album %5,name %5 LIMIT 1").arg(filter, condition, group, key, direction));
    stmt.bind(1, seed).bind(2, local ? db::Value(currentAlbum) : db::Value()).bind(3, qint64(query.favoritesOnly)).bind(4, value).bind(5, currentAlbum).bind(6, currentName).bind(7, label);
    if (stmt.step()) {
      const QString album = stmt.text(0);
      return itemFor(root, album, stmt.text(1), stmt.text(2), stmt.integer(3) != 0, stmt.optionalText(4), lastComponent(album), stmt.optionalText(5));
    }
  }
  return std::nullopt;
}

QJsonArray nativeGeometry(const PageQuery &query) {
  db::Connection conn = open(query.rootDir);
  const QString direction = query.sortDir == QLatin1String("desc") ? QStringLiteral("DESC") : QStringLiteral("ASC");
  const QString order = orderKey(query.sortKey, 3);
  const QString filter = query.albumId ? QStringLiteral("album=?1 AND (?2=0 OR favorite=1)") : QStringLiteral("?1 IS NULL AND (?2=0 OR favorite=1)");
  const QString grouping = query.albumId ? QString() : QStringLiteral("album_label(album) ASC, album ASC, ");
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT meta,album,name,favorite,label FROM media WHERE %1 AND (?3 IS NOT NULL) ORDER BY %2%3 %4, album %4, name %4").arg(filter, grouping, order, direction));
  stmt.bind(1, optional(query.albumId)).bind(2, qint64(query.favoritesOnly)).bind(3, seedOf(query.randomSeed));
  QJsonArray result;
  while (stmt.step()) {
    const DetachedFileMeta meta = unpackFileMeta(stmt.text(0));
    const QString album = stmt.text(1), name = stmt.text(2);
    result.append(QJsonObject{{QStringLiteral("name"), name},
                              {QStringLiteral("revision"), metadata::mediaRevision(path::join(path::join(query.rootDir, album), name))},
                              {QStringLiteral("width"), qint64(meta.w.value_or(1))},
                              {QStringLiteral("height"), qint64(meta.h.value_or(1))},
                              {QStringLiteral("albumId"), album},
                              {QStringLiteral("albumName"), lastComponent(album)},
                              {QStringLiteral("favorite"), stmt.integer(3) != 0},
                              {QStringLiteral("displayName"), stmt.isNull(4) ? QJsonValue() : QJsonValue(stmt.text(4))}});
  }
  return result;
}

void releaseMemory(const QString &root) {
  std::lock_guard lock(memoryMutex);
  for (auto it = memoryKeepers.begin(); it != memoryKeepers.end();) {
    if (it->first == root || path::startsWith(it->first, root)) it = memoryKeepers.erase(it);
    else ++it;
  }
}

}  // namespace core::catalog
