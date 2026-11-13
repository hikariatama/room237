#include "album_db.h"

#include "catalog.h"
#include "vault.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace core::album_db {

namespace {
// Rows of an album inside a vault root are sealed with the root's row key, bound to their file name.
struct Codec {
  std::shared_ptr<const vault::Keys> keys;
  bool vault = false;
  QString encode(const QString &name, const FileMetaEntry &entry) const {
    if (!vault) return entry.toJson();
    if (!keys) fail(QStringLiteral("This library is locked"));
    return QStringLiteral("enc1:") + QString::fromLatin1(vault::seal(keys->rows, entry.toJson().toUtf8(), name.toUtf8()).toBase64());
  }
  FileMetaEntry decode(const QString &name, const QString &data) const {
    if (!data.startsWith(QLatin1String("enc1:"))) return FileMetaEntry::fromJson(data);
    if (!keys) fail(QStringLiteral("This library is locked"));
    const auto plain = vault::open(keys->rows, QByteArray::fromBase64(data.mid(5).toLatin1()), name.toUtf8());
    if (!plain) fail(QStringLiteral("Album metadata belongs to another key or is damaged"));
    return FileMetaEntry::fromJson(QString::fromUtf8(*plain));
  }
};
thread_local const Codec *current = nullptr;
Codec codecFor(const QString &databasePath) {
  Codec codec;
  const QString album = path::parent(path::parent(databasePath));
  codec.keys = vault::keysFor(album, &codec.vault);
  return codec;
}
class Scope {
public:
  explicit Scope(const QString &databasePath) : codec_(codecFor(databasePath)), previous_(current) { current = &codec_; }
  explicit Scope(const Codec &codec) : codec_(codec), previous_(current) { current = &codec_; }
  ~Scope() { current = previous_; }
private:
  Codec codec_;
  const Codec *previous_;
};
const Codec &codec() {
  static const Codec plain;
  return current ? *current : plain;
}
}  // namespace

db::Connection connect(const QString &path, bool writable) {
  // Readers open read/write when allowed so SQLite can recover a hot rollback journal left by a killed writer.
  // Fall back for read-only albums.
  std::optional<db::Connection> opened;
  try {
    opened.emplace(path, db::Open::ReadWrite);
  } catch (const Error &) {
    if (writable) throw;
    opened.emplace(path, db::Open::ReadOnly);
  }
  db::Connection conn = std::move(*opened);
  conn.busyTimeout(5000);
  // Deleted rows are overwritten, so a renamed or removed file leaves no readable record behind in free pages.
  conn.executeBatch(QStringLiteral("PRAGMA secure_delete=ON"));
  const qint64 version = conn.pragmaInteger(QStringLiteral("user_version"));
  if (version == 1 && !conn.readOnly()) {
    db::Transaction tx(conn, db::Behavior::Immediate);
    // Another connection may have completed the migration while we waited.
    const qint64 now = conn.pragmaInteger(QStringLiteral("user_version"));
    if (now == 1) {
      conn.executeBatch(QStringLiteral(
          "CREATE TABLE ignored_pairs(a TEXT NOT NULL,b TEXT NOT NULL,PRIMARY KEY(a,b));"
          "CREATE INDEX ignored_pairs_b ON ignored_pairs(b);"
          "INSERT OR IGNORE INTO ignored_pairs SELECT json_extract(value,'$[0]'),json_extract(value,'$[1]') FROM album,json_each(album.ignores);"
          "CREATE INDEX IF NOT EXISTS files_heic_output ON files(json_extract(data,'$.heic_conversion.output_name'));"
          "UPDATE album SET ignores='[]'; PRAGMA user_version=2;"));
    } else if (now != 2) {
      fail(QStringLiteral("Unsupported album database version: %1").arg(now));
    }
    tx.commit();
  } else if (version != 2) {
    fail(QStringLiteral("Unsupported album database version: %1").arg(version));
  }
  // Sealed rows are not JSON, so an encrypted root's databases cannot keep the expression index.
  if (!conn.readOnly()) {
    if (vault::isVaultPath(path::parent(path::parent(path)))) conn.executeBatch(QStringLiteral("DROP INDEX IF EXISTS files_heic_output"));
    else conn.executeBatch(QStringLiteral("CREATE INDEX IF NOT EXISTS files_heic_output ON files(json_extract(data,'$.heic_conversion.output_name'))"));
  }
  return conn;
}

void putRows(db::Connection &conn, const Rows &rows) {
  db::Statement stmt = conn.prepare(QStringLiteral("INSERT INTO files(name, data) VALUES (?1, ?2) ON CONFLICT(name) DO UPDATE SET data=excluded.data WHERE files.data<>excluded.data"));
  for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
    stmt.reset();
    stmt.bind(1, it.key()).bind(2, codec().encode(it.key(), it.value())).execute();
  }
}

static void initializeWith(const QString &path, const std::function<void(db::Connection &)> &populate) {
  // Publish only a complete database. A crash before publication leaves the legacy metadata authoritative.
  const QString dir = path::parent(path);
  if (dir.isEmpty()) fail(QStringLiteral("Missing database directory"));
  TempFile temporary(dir, QStringLiteral(".room237-db-"));
  {
    db::Connection conn(temporary.path());
    // Rollback journal keeps the album portable without a persistent WAL file.
    conn.executeBatch(QStringLiteral(
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
        "CREATE TABLE files (name TEXT PRIMARY KEY NOT NULL, data TEXT NOT NULL);"
        "CREATE TABLE album (id INTEGER PRIMARY KEY CHECK(id=1), ignores TEXT NOT NULL);"
        "CREATE INDEX files_heic_output ON files(json_extract(data,'$.heic_conversion.output_name'));"
        "CREATE TABLE ignored_pairs(a TEXT NOT NULL,b TEXT NOT NULL,PRIMARY KEY(a,b));"
        "CREATE INDEX ignored_pairs_b ON ignored_pairs(b);"
        "PRAGMA user_version=2;"));
    db::Transaction tx(conn);
    conn.execute(QStringLiteral("INSERT INTO album VALUES(1,'[]')"));
    populate(conn);
    tx.commit();
    conn.close();
  }
  temporary.sync();
  temporary.persistNoClobber(path);
#ifndef Q_OS_WIN
  syncDir(dir);
#endif
}

void initialize(const QString &path, const AlbumMeta &album) {
  Scope scope(path);
  initializeWith(path, [&](db::Connection &conn) {
    putRows(conn, album.files);
    for (const auto &[a, b] : album.duplicatesIgnore) conn.execute(QStringLiteral("INSERT OR IGNORE INTO ignored_pairs VALUES(?1,?2)"), {a, b});
  });
}

void initializeLegacy(const QString &path) {
  Scope scope(path);
  initializeWith(path, [&](db::Connection &conn) {
    const QString dir = path::parent(path);
    if (dir.isEmpty()) fail(QStringLiteral("Missing metadata directory"));
    const QString legacy = path::join(dir, QStringLiteral("album.json"));
    if (exists(legacy)) {
      QJsonParseError error;
      const QJsonDocument document = QJsonDocument::fromJson(readAll(legacy), &error);
      if (error.error != QJsonParseError::NoError || !document.isObject()) fail(QStringLiteral("Invalid metadata in %1: %2").arg(legacy, error.errorString()));
      const QJsonObject album = document.object();
      try {
        const QJsonValue files = album.value(QLatin1String("files"));
        if (!files.isUndefined()) {
          if (!files.isObject()) fail(QStringLiteral("invalid type: expected a map"));
          const QJsonObject map = files.toObject();
          for (auto it = map.begin(); it != map.end(); ++it) {
            if (!it.value().isObject()) fail(QStringLiteral("invalid type: expected struct FileMetaEntry"));
            putRows(conn, Rows{{it.key(), FileMetaEntry::fromJsonObject(it.value().toObject())}});
          }
        }
        const QJsonValue pairs = album.value(QLatin1String("duplicates_ignore"));
        if (!pairs.isUndefined()) {
          if (!pairs.isArray()) fail(QStringLiteral("invalid type: expected a sequence"));
          for (const QJsonValue &pair : pairs.toArray()) {
            const QJsonArray items = pair.toArray();
            if (!pair.isArray() || items.size() != 2 || !items[0].isString() || !items[1].isString()) fail(QStringLiteral("invalid ignored pair"));
            conn.execute(QStringLiteral("INSERT OR IGNORE INTO ignored_pairs VALUES(?1,?2)"), {items[0].toString(), items[1].toString()});
          }
        }
      } catch (const Error &e) {
        fail(QStringLiteral("Invalid metadata in %1: %2").arg(legacy, e.message()));
      }
    } else {
      for (const DirEntry &file : readDir(dir)) {
        if (!file.isFile || !file.name.endsWith(QLatin1String(".meta"))) continue;
        const QString name = file.name.chopped(5);
        putRows(conn, Rows{{name, FileMetaEntry::fromJson(QString::fromUtf8(readAll(path::join(dir, file.name))))}});
      }
    }
  });
}

static std::optional<FileMetaEntry> getRow(db::Connection &conn, const QString &name) {
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT data FROM files WHERE name=?1"));
  stmt.bind(1, name);
  if (!stmt.step()) return std::nullopt;
  return codec().decode(name, stmt.text(0));
}

FileReader::FileReader(const QString &path) : conn_(connect(path, false)), path_(path) {}
std::optional<FileMetaEntry> FileReader::get(const QString &name) {
  Scope scope(path_);
  return getRow(conn_, name);
}

AlbumMeta subset(const QString &path, const QStringList &names, bool related) {
  Scope scope(path);
  db::Connection conn = connect(path, false);
  AlbumMeta album;
  for (const QString &name : names) {
    if (auto entry = getRow(conn, name)) album.files.insert(name, *entry);
    if (related && codec().vault) {
      db::Statement stmt = conn.prepare(QStringLiteral("SELECT name,data FROM files"));
      while (stmt.step()) {
        const FileMetaEntry entry = codec().decode(stmt.text(0), stmt.text(1));
        if (entry.heicConversion && entry.heicConversion->outputName == name) album.files.insert(stmt.text(0), entry);
      }
    } else if (related) {
      db::Statement stmt = conn.prepare(QStringLiteral("SELECT name,data FROM files WHERE json_extract(data,'$.heic_conversion.output_name')=?1"));
      stmt.bind(1, name);
      while (stmt.step()) album.files.insert(stmt.text(0), codec().decode(stmt.text(0), stmt.text(1)));
    }
  }
  return album;
}

void publishRows(const QString &path, const Rows &rows) {
  const QString album = path::parent(path::parent(path));
  if (album.isEmpty()) return;
  for (auto it = rows.cbegin(); it != rows.cend(); ++it) catalog::publish(album, it.key(), it.value());
}

void patch(const QString &path, const QStringList &names, const AlbumMeta &album) {
  Scope scope(path);
  db::Connection conn = connect(path, true);
  db::Transaction tx(conn);
  for (const QString &name : names) {
    auto it = album.files.constFind(name);
    if (it != album.files.cend()) putRows(conn, Rows{{name, *it}});
    else conn.execute(QStringLiteral("DELETE FROM files WHERE name=?1"), {name});
  }
  tx.commit();
  const QString dir = path::parent(path::parent(path));
  if (!dir.isEmpty())
    for (const QString &name : names) catalog::publish(dir, name, album.files.value(name));
}

void remove(const QString &path, const QStringList &names) {
  db::Connection conn = connect(path, true);
  db::Transaction tx(conn);
  for (const QString &name : names) {
    conn.execute(QStringLiteral("DELETE FROM files WHERE name=?1"), {name});
    conn.execute(QStringLiteral("DELETE FROM ignored_pairs WHERE a=?1 OR b=?1"), {name});
  }
  tx.commit();
}

void ignorePairs(const QString &path, const QStringList &names) {
  db::Connection conn = connect(path, true);
  db::Transaction tx(conn);
  for (int i = 0; i < names.size(); ++i)
    for (int j = i + 1; j < names.size(); ++j) conn.execute(QStringLiteral("INSERT OR IGNORE INTO ignored_pairs VALUES(?1,?2)"), {names[i], names[j]});
  tx.commit();
}

bool clearIgnores(const QString &path) {
  db::Connection conn = connect(path, true);
  return conn.execute(QStringLiteral("DELETE FROM ignored_pairs")) > 0;
}

std::optional<FileMetaEntry> readFile(const QString &path, const QString &name) {
  Scope scope(path);
  db::Connection conn = connect(path, false);
  return getRow(conn, name);
}

static AlbumMeta readConn(db::Connection &conn) {
  AlbumMeta album;
  db::Statement files = conn.prepare(QStringLiteral("SELECT name, data FROM files"));
  while (files.step()) album.files.insert(files.text(0), codec().decode(files.text(0), files.text(1)));
  db::Statement pairs = conn.prepare(QStringLiteral("SELECT a,b FROM ignored_pairs ORDER BY rowid"));
  while (pairs.step()) album.duplicatesIgnore.emplace_back(pairs.text(0), pairs.text(1));
  return album;
}

AlbumMeta read(const QString &path) {
  Scope scope(path);
  db::Connection conn = connect(path, false);
  db::Transaction tx(conn);
  return readConn(conn);
}

void replace(const QString &path, const AlbumMeta &album) {
  Scope scope(path);
  db::Connection conn = connect(path, true);
  db::Transaction tx(conn, db::Behavior::Immediate);
  const AlbumMeta previous = readConn(conn);
  QStringList deleted;
  for (auto it = previous.files.cbegin(); it != previous.files.cend(); ++it)
    if (!album.files.contains(it.key())) {
      conn.execute(QStringLiteral("DELETE FROM files WHERE name=?1"), {it.key()});
      deleted << it.key();
    }
  Rows changed;
  for (auto it = album.files.cbegin(); it != album.files.cend(); ++it) {
    auto old = previous.files.constFind(it.key());
    if (old == previous.files.cend() || !(*old == *it)) changed.insert(it.key(), *it);
  }
  putRows(conn, changed);
  if (previous.duplicatesIgnore != album.duplicatesIgnore) {
    conn.execute(QStringLiteral("DELETE FROM ignored_pairs"));
    for (const auto &[a, b] : album.duplicatesIgnore) conn.execute(QStringLiteral("INSERT OR IGNORE INTO ignored_pairs VALUES(?1,?2)"), {a, b});
  }
  tx.commit();
  const QString dir = path::parent(path::parent(path));
  if (!dir.isEmpty()) {
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) catalog::publish(dir, it.key(), it.value());
    catalog::forget(dir, deleted);
  }
}

void updateRowsVoid(const QString &path, const std::function<void(db::Connection &, Rows &)> &update) {
  Scope scope(path);
  db::Connection conn = connect(path, true);
  db::Transaction tx(conn, db::Behavior::Immediate);
  Rows rows;
  update(conn, rows);
  putRows(conn, rows);
  tx.commit();
  publishRows(path, rows);
}

FileMetaEntry entry(db::Connection &conn, const QString &name) { return getRow(conn, name).value_or(FileMetaEntry()); }

void reseal(const QString &path, bool seal, const QHash<QString, QString> &renames) {
  Codec reading = codecFor(path);
  Codec writing = reading;
  writing.vault = seal;
  if (seal && !writing.keys) fail(QStringLiteral("This library is locked"));
  db::Connection conn = connect(path, true);
  db::Transaction tx(conn, db::Behavior::Immediate);
  Rows rows;
  {
    Scope scope(reading);
    db::Statement stmt = conn.prepare(QStringLiteral("SELECT name,data FROM files"));
    while (stmt.step()) rows.insert(renames.value(stmt.text(0), stmt.text(0)), reading.decode(stmt.text(0), stmt.text(1)));
  }
  conn.execute(QStringLiteral("DELETE FROM files"));
  {
    Scope scope(writing);
    putRows(conn, rows);
  }
  for (auto it = renames.cbegin(); it != renames.cend(); ++it) {
    conn.execute(QStringLiteral("UPDATE OR IGNORE ignored_pairs SET a=?2 WHERE a=?1"), {it.key(), it.value()});
    conn.execute(QStringLiteral("UPDATE OR IGNORE ignored_pairs SET b=?2 WHERE b=?1"), {it.key(), it.value()});
  }
  tx.commit();
  conn.executeBatch(QStringLiteral("VACUUM"));
}

void renameRow(const QString &path, const QString &from, const QString &to, const FileMetaEntry &value) {
  Scope scope(path);
  db::Connection conn = connect(path, true);
  db::Transaction tx(conn, db::Behavior::Immediate);
  conn.execute(QStringLiteral("DELETE FROM files WHERE name=?1"), {from});
  putRows(conn, Rows{{to, value}});
  conn.execute(QStringLiteral("UPDATE OR IGNORE ignored_pairs SET a=?2 WHERE a=?1"), {from, to});
  conn.execute(QStringLiteral("UPDATE OR IGNORE ignored_pairs SET b=?2 WHERE b=?1"), {from, to});
  conn.execute(QStringLiteral("DELETE FROM ignored_pairs WHERE a=?1 OR b=?1"), {from});
  tx.commit();
}

}  // namespace core::album_db
