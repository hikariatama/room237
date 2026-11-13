#pragma once
// One portable database per album. Connections are short-lived so moving or clearing an album never leaves an open
// handle to an obsolete database.
#include "db.h"
#include "entry.h"

#include <functional>
#include <memory>

namespace core::album_db {

class FileReader {
public:
  explicit FileReader(const QString &path);
  std::optional<FileMetaEntry> get(const QString &name);
private:
  db::Connection conn_;
  QString path_;
};

void initialize(const QString &path, const AlbumMeta &album);  // Tests.
void initializeLegacy(const QString &path);
AlbumMeta subset(const QString &path, const QStringList &names, bool related);
void patch(const QString &path, const QStringList &names, const AlbumMeta &album);
void remove(const QString &path, const QStringList &names);
void ignorePairs(const QString &path, const QStringList &names);
bool clearIgnores(const QString &path);
std::optional<FileMetaEntry> readFile(const QString &path, const QString &name);
AlbumMeta read(const QString &path);
void replace(const QString &path, const AlbumMeta &album);  // Tests.

using Rows = QHash<QString, FileMetaEntry>;
// The hot path touches only the requested primary keys, even in a huge album.
template <typename T> T updateRows(const QString &path, const std::function<T(db::Connection &, Rows &)> &update);
void updateRowsVoid(const QString &path, const std::function<void(db::Connection &, Rows &)> &update);
FileMetaEntry entry(db::Connection &conn, const QString &name);
// Rewrites every row sealed (`seal`) or in the clear, renaming rows and ignored pairs per `renames` (old -> new).
void reseal(const QString &path, bool seal, const QHash<QString, QString> &renames = {});
// Moves one row to a new name in a single transaction, with its ignored pairs.
void renameRow(const QString &path, const QString &from, const QString &to, const FileMetaEntry &entry);

db::Connection connect(const QString &path, bool writable);
void putRows(db::Connection &conn, const Rows &rows);
void publishRows(const QString &path, const Rows &rows);

template <typename T> T updateRows(const QString &path, const std::function<T(db::Connection &, Rows &)> &update) {
  std::optional<T> result;
  updateRowsVoid(path, [&](db::Connection &conn, Rows &rows) { result.emplace(update(conn, rows)); });
  return std::move(*result);
}

}  // namespace core::album_db
