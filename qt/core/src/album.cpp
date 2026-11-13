#include "album.h"

#include "album_tree.h"
#include "catalog.h"
#include "constants.h"
#include "duplicates.h"
#include "json.h"
#include "media_delete.h"
#include "media_move.h"
#include "metadata.h"
#include "preload.h"
#include "thumb.h"
#include "util.h"

#include <QFile>
#include <QJsonArray>
#include <QSet>
#include <QtConcurrent>
#include <thread>

#ifndef Q_OS_WIN
#include <cerrno>
#include <sys/stat.h>
#else
#include <windows.h>
#endif

namespace core::album {

namespace {
struct AlbumDirEntry {
  QString path, name, relativePath;
  std::optional<QString> parent;
};

QString normalized(const QString &relative) { return QString(relative).replace(QLatin1Char('\\'), QLatin1Char('/')); }

// lstat that tells a name this filesystem rejects as too long apart from an absent one.
bool occupied(const QString &p, bool *tooLong) {
  *tooLong = false;
#ifndef Q_OS_WIN
  struct stat st {};
  if (::lstat(QFile::encodeName(p).constData(), &st) == 0) return true;
  if (errno == ENAMETOOLONG) *tooLong = true;
  else if (errno != ENOENT && errno != ENOTDIR) fail(QString::fromLocal8Bit(strerror(errno)));
  return false;
#else
  WIN32_FILE_ATTRIBUTE_DATA data {};
  if (GetFileAttributesExW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(p).utf16()), GetFileExInfoStandard, &data)) return true;
  const DWORD error = GetLastError();
  if (error == ERROR_FILENAME_EXCED_RANGE) *tooLong = true;
  else if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) fail(QStringLiteral("Windows error %1").arg(error));
  return false;
#endif
}
bool filenameOccupied(const QString &p) {
  bool tooLong = false;
  const bool taken = occupied(p, &tooLong);
  if (tooLong) fail(QStringLiteral("File name too long (os error 63)"));
  return taken;
}

std::optional<QString> collisionFilename(const QString &dir, const QString &base, const QString &suffix, const QString &extension) {
  QString candidate = base + suffix + extension;
  bool tooLong = false;
  if (!occupied(path::join(dir, candidate), &tooLong) && !tooLong) return candidate;
  if (!tooLong) return std::nullopt;
  // Only shorten a name rejected by this filesystem. macOS may accept a Unicode name with more than 255 UTF-8 bytes.
  const QByteArray baseBytes = base.toUtf8();
  const int fixed = int(suffix.toUtf8().size() + extension.toUtf8().size());
  if (fixed > 255) fail(QStringLiteral("Filename suffix exceeds the component limit"));
  int boundary = std::min<int>(255 - fixed, int(baseBytes.size()));
  while (boundary > 0 && (uchar(baseBytes[boundary]) & 0xc0) == 0x80) --boundary;
  if (boundary == 0) fail(QStringLiteral("Filename suffix leaves no space for its stem"));
  candidate = QString::fromUtf8(baseBytes.left(boundary)) + suffix + extension;
  if (filenameOccupied(path::join(dir, candidate))) return std::nullopt;
  return candidate;
}

// Rust's Path::file_stem/extension: a leading dot does not start an extension.
std::pair<QString, QString> stemAndExtension(const QString &name) {
  const int dot = name.lastIndexOf(QLatin1Char('.'));
  if (dot <= 0) return {name, QString()};
  return {name.left(dot), name.mid(dot + 1)};
}
std::pair<QString, quint64> counterBase(const QString &stem) {
  const int underscore = stem.lastIndexOf(QLatin1Char('_'));
  if (underscore >= 0) {
    bool ok = false;
    const QString digits = stem.mid(underscore + 1);
    const quint64 n = digits.toULongLong(&ok);
    if (ok && !digits.isEmpty() && !digits.startsWith(QLatin1Char('-')) && !digits.startsWith(QLatin1Char(' '))) return {stem.left(underscore), n};
  }
  return {stem, 0};
}

QString uniqueFilenameReserved(const QString &dir, const QString &fileName, const QSet<QString> &reserved) {
  const auto [stem, ext] = stemAndExtension(fileName);
  const QString extension = ext.isEmpty() ? QString() : QLatin1Char('.') + ext;
  const auto [base, start] = counterBase(stem);
  if (!reserved.contains(fileName) && !filenameOccupied(path::join(dir, fileName))) return fileName;
  if (start == std::numeric_limits<quint64>::max()) fail(QStringLiteral("Filename counter exhausted"));
  quint64 counter = start == 0 ? 1 : start + 1;
  for (;;) {
    if (const auto candidate = collisionFilename(dir, base, QStringLiteral("_%1").arg(counter), extension); candidate && !reserved.contains(*candidate)) return *candidate;
    if (counter == std::numeric_limits<quint64>::max()) fail(QStringLiteral("Filename counter exhausted"));
    ++counter;
  }
}

QString nextAvailableName(const QString &dir, const QString &name) {
  if (!filenameOccupied(path::join(dir, name))) return name;
  auto [stem, ext] = stemAndExtension(name);
  if (stem.isEmpty()) stem = QStringLiteral("file");
  const auto [base, start] = counterBase(stem);
  quint64 counter = start;
  for (;;) {
    const QString suffix = counter == 0 ? QString() : QStringLiteral("_%1").arg(counter);
    if (const auto candidate = collisionFilename(dir, base, suffix, ext.isEmpty() ? QString() : QLatin1Char('.') + ext)) return *candidate;
    if (counter == std::numeric_limits<quint64>::max()) fail(QStringLiteral("Filename counter exhausted"));
    ++counter;
  }
}

std::vector<AlbumDirEntry> walkAlbumEntries(const QString &root) {
  std::vector<AlbumDirEntry> entries;
  for (const QString &p : walkAlbumPaths(root)) {
    const QString name = path::fileName(p);
    if (name.isEmpty()) continue;
    if (!path::startsWith(p, root)) fail(QStringLiteral("%1 is not under root %2").arg(p, root));
    const QString relative = path::relative(p, root);
    const QString parent = normalized(path::parent(relative));
    entries.push_back({p, name, normalized(relative), parent.isEmpty() ? std::nullopt : std::optional(parent)});
  }
  std::sort(entries.begin(), entries.end(), [](const AlbumDirEntry &a, const AlbumDirEntry &b) { return a.relativePath < b.relativePath; });
  return entries;
}

QString fallbackMediaMetadata(const QString &p) {
  const FileInfo info = stat(p);
  DetachedFileMeta meta;
  if (info.birthSec && *info.birthSec >= 0) meta.a = quint64(*info.birthSec);
  meta.s = metadata::readEmbeddedTimestamp(p);
  meta.i = constants::isImage(p);
  meta.v = constants::isVideo(p);
  return meta.pack();
}

struct AlbumTreeRenameGuard {
  std::unique_lock<std::mutex> moving;
  std::unique_ptr<album_tree::TreeLease> tree;
};

AlbumTreeRenameGuard prepareAlbumTreeRename(const QString &root, const QString &dir) {
  album_tree::initialize(root);
  for (int attempt = 0; attempt < 3; ++attempt) {
    QStringList albums = walkAlbumPaths(dir);
    albums << dir;
    std::sort(albums.begin(), albums.end());
    for (const QString &album : albums) media_delete::recoverAlbum(album);
    auto tree = album_tree::write(root);
    // Bootstrap a newly created root marker against operations already in flight, and freeze journal recovery and
    // metadata writers as well.
    for (const QString &album : albums) {
      auto maintenance = media_delete::lockMaintenance(album);
      auto metadataLease = metadata::lockAlbumMetadata(album);
    }
    std::unique_lock moving(mediaMoveLock);
    // A move can finish or leave recovery work while the mutex is awaited. Recheck the current subtree before changing
    // the journal's paths.
    QStringList current = walkAlbumPaths(dir);
    current << dir;
    std::sort(current.begin(), current.end());
    bool pending = false;
    for (const QString &album : current) pending |= media_move::hasJournal(album);
    if (current == albums && !pending) return {std::move(moving), std::move(tree)};
  }
  fail(QStringLiteral("Album has pending media moves; retry after they finish"));
}

using MoveGroup = std::vector<std::pair<QString, std::optional<QString>>>;
struct PlannedMove {
  QString source, target, revision;
  bool operator==(const PlannedMove &) const = default;
};

std::vector<PlannedMove> planMoveGroup(const QString &sourceDir, const QString &targetDir, const MoveGroup &group, QSet<QString> &reserved) {
  std::vector<PlannedMove> planned;
  try {
    for (const auto &[name, expected] : group) {
      const QString source = path::join(sourceDir, name);
      if (!lstat(source).isFile) fail(QStringLiteral("Move source is not a regular file"));
      const auto revision = metadata::sourceRevision(source);
      if (!revision) fail(QStringLiteral("Move source unavailable"));
      if (expected && *expected != *revision) fail(QStringLiteral("Bound media changed before move"));
      const QString target = uniqueFilenameReserved(targetDir, name, reserved);
      reserved.insert(target);
      planned.push_back({name, target, *revision});
    }
  } catch (const Error &) {
    for (const PlannedMove &move : planned) reserved.remove(move.target);
    throw;
  }
  return planned;
}

QHash<QString, MoveGroup> logicalMoveGroups(const QString &dir, const QStringList &requested, const AlbumMeta &snapshot) {
  QSet<QString> outputs(requested.begin(), requested.end());
  for (const QString &name : requested)
    if (auto it = snapshot.files.constFind(name); it != snapshot.files.cend() && it->heicConversion) outputs.insert(it->heicConversion->outputName);
  QHash<QString, MoveGroup> byOutput;
  for (auto it = snapshot.files.cbegin(); it != snapshot.files.cend(); ++it) {
    if (!it->heicConversion) continue;
    const HeicConversion &binding = *it->heicConversion;
    if (outputs.contains(binding.outputName) && metadata::heicBindingCurrent(dir, it.key(), binding)) {
      MoveGroup &group = byOutput[binding.outputName];
      if (group.empty()) group.emplace_back(binding.outputName, binding.outputRevision);
      group.emplace_back(it.key(), binding.sourceRevision);
    }
  }
  for (MoveGroup &group : byOutput) std::sort(group.begin() + 1, group.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  QHash<QString, MoveGroup> groups;
  for (const QString &name : requested) {
    std::optional<MoveGroup> chosen;
    auto it = snapshot.files.constFind(name);
    if (it != snapshot.files.cend() && it->heicConversion && byOutput.contains(it->heicConversion->outputName)) {
      const MoveGroup &raw = byOutput[it->heicConversion->outputName];
      if (std::any_of(raw.begin(), raw.end(), [&](const auto &member) { return member.first == name; })) chosen = raw;
    }
    if (!chosen && byOutput.contains(name)) chosen = byOutput.value(name);
    groups.insert(name, chosen ? *chosen : MoveGroup{{name, std::nullopt}});
  }
  return groups;
}

void rollbackMovedFiles(const QString &source, const QString &target, const std::vector<PlannedMove> &records) {
  QStringList errors;
  for (auto it = records.rbegin(); it != records.rend(); ++it) {
    const QString destination = path::join(target, it->target);
    if (metadata::sourceRevision(destination) != it->revision) {
      errors << QStringLiteral("Moved file changed; preserving %1").arg(destination);
      continue;
    }
    QString error;
    if (!renameNoClobber(destination, path::join(source, it->source), &error)) errors << QStringLiteral("Could not restore %1: %2").arg(it->source, error);
  }
  if (!errors.isEmpty()) fail(errors.join(QStringLiteral("; ")));
}

ImportResult addMediaFilesBlocking(const QString &dir, const std::vector<IncomingFile> &files) {
  if (!isDir(dir)) fail(QStringLiteral("%1 is not a directory").arg(dir));
  auto tree = album_tree::read({dir});
  media_delete::recoverAlbum(dir);
  for (const IncomingFile &file : files) validateFileName(file.name.trimmed());
  QStringList copied;
  ImportResult result;
  {
    auto maintenance = media_delete::lockMaintenance(dir);
    tree->ensureCurrent({dir});
    std::lock_guard moving(mediaMoveLock);
    createDerivedDir(path::join(dir, thumb::kThumbDir));
    auto persist = [&](TempFile &temporary, const QString &name) {
      for (int attempt = 0; attempt < 1000; ++attempt) {
        const QString targetName = nextAvailableName(dir, name);
        QString error;
        if (renameNoClobber(temporary.path(), path::join(dir, targetName), &error)) {
          temporary.release();
          return targetName;
        }
        if (!entryExists(path::join(dir, targetName))) fail(error);
      }
      fail(QStringLiteral("Too many concurrent filename collisions"));
    };
    for (const IncomingFile &file : files) {
      const QString name = file.name.trimmed();
      try {
        if (file.sourcePath) {
          const QString source = *file.sourcePath;
          const FileInfo info = stat(source);
          if (!info.exists) fail(QStringLiteral("No such file or directory (os error 2)"));
          if (!info.isFile) fail(QStringLiteral("Import source is not a regular file"));
          const QString parent = path::parent(source);
          if (path::startsWith(parent, dir) && path::startsWith(dir, parent)) {
            const QString own = path::fileName(source);
            if (own.isEmpty()) fail(QStringLiteral("Invalid source filename"));
            copied << own;
            continue;
          }
          TempFile temporary(dir, QStringLiteral(".tmp"));
          QFile in(source);
          if (!in.open(QIODevice::ReadOnly)) fail(in.errorString());
          QFile out(temporary.path());
          if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) fail(out.errorString());
          QByteArray chunk;
          while (!(chunk = in.read(1 << 20)).isEmpty())
            if (out.write(chunk) != chunk.size()) fail(out.errorString());
          if (in.error() != QFileDevice::NoError) fail(in.errorString());
          out.close();
          temporary.sync();
          copied << persist(temporary, name);
        } else if (file.data) {
          TempFile temporary(dir, QStringLiteral(".tmp"));
          temporary.write(*file.data);
          temporary.sync();
          copied << persist(temporary, name);
        } else {
          fail(QStringLiteral("Import file has no data or source"));
        }
      } catch (const Error &error) {
        result.failed.emplace_back(name, error.message());
      }
    }
  }
  // A raw HEIC and its in-album JPEG can register the same visible file. Preserve first occurrence order.
  QSet<QString> registered;
  QStringList unique;
  for (const QString &name : copied)
    if (!registered.contains(name)) {
      registered.insert(name);
      unique << name;
    }
  AlbumMeta albumMeta;
  try {
    albumMeta = metadata::readSelectedMetadata(dir, unique, false);
  } catch (const Error &error) {
    qWarning("Import metadata unavailable: %s", qUtf8Printable(error.message()));
  }
  const AdvancedSettings settings = readSettings();
  const QList<std::optional<DetachedMediaEntry>> added = QtConcurrent::blockingMapped(unique, [&](const QString &targetName) -> std::optional<DetachedMediaEntry> {
    const QString file = path::join(dir, targetName);
    if (!isFile(file)) {
      qWarning("add_media_files: %s is not a file", qUtf8Printable(file));
      return std::nullopt;
    }
    try {
      thumb::ensureThumbWithSettings(file, path::join(dir, thumb::kThumbDir), settings);
    } catch (const std::exception &) {
    }
    QString meta;
    try {
      meta = metadata::getFileMetadataCached(file);
    } catch (const std::exception &error) {
      qWarning("add_media_files: failed to get metadata for %s: %s", qUtf8Printable(targetName), error.what());
      // The original has already been published. Return it to the interface even if derived data is unavailable.
      meta = fallbackMediaMetadata(file);
    }
    try {
      if (!duplicates::computeHashForPath(albumMeta, file, settings.hashConfig(), settings)) qInfo("hash not computed for %s", qUtf8Printable(file));
    } catch (const std::exception &) {
    }
    DetachedMediaEntry entry;
    try {
      if (auto stored = metadata::readFileEntry(file)) entry.blurPreview = stored->blurPreview;
    } catch (const std::exception &) {
    }
    entry.revision = metadata::mediaRevision(file);
    entry.thumbnailName = thumb::detachedThumbnailName(file);
    entry.meta = meta;
    entry.name = path::fileName(file);
    if (albumMeta.files.value(targetName).favorite) entry.favorite = true;
    return entry;
  });
  for (const auto &entry : added)
    if (entry) result.added.push_back(*entry);
  return result;
}
}  // namespace

bool isAlbumDir(const QString &p) {
  const FileInfo entry = lstat(p);
  if (!entry.exists || entry.isSymlink || !entry.isDir) return false;
  const QString name = path::fileName(p);
  return !name.isEmpty() && !name.startsWith(QLatin1String(".room237-"));
}

QStringList walkAlbumPaths(const QString &root) {
  if (!isDir(root)) fail(QStringLiteral("%1 is not a directory").arg(root));
  QStringList albums, pending{root};
  while (!pending.isEmpty()) {
    const QString dir = pending.takeLast();
    for (const DirEntry &entry : readDir(dir)) {
      const QString p = path::join(dir, entry.name);
      if (!isAlbumDir(p)) continue;
      albums << p;
      pending << p;
    }
  }
  return albums;
}

QJsonObject RenamedAlbumResult::toJson() const {
  QJsonObject o{{QStringLiteral("oldPath"), oldPath}, {QStringLiteral("newPath"), newPath}, {QStringLiteral("oldRelativePath"), oldRelativePath}, {QStringLiteral("newRelativePath"), newRelativePath}, {QStringLiteral("name"), name}};
  if (parent) o.insert(QStringLiteral("parent"), *parent);
  return o;
}

std::vector<IncomingFile> IncomingFile::fromJson(const QJsonValue &value) {
  if (!value.isArray()) fail(QStringLiteral("files: invalid type, expected a sequence"));
  std::vector<IncomingFile> files;
  for (const QJsonValue &item : value.toArray()) {
    const QJsonObject o = item.toObject();
    IncomingFile file;
    file.name = json::stringField(o.value(QLatin1String("name")), "name");
    const QJsonValue source = o.value(QLatin1String("sourcePath"));
    if (!source.isNull() && !source.isUndefined()) file.sourcePath = json::stringField(source, "sourcePath");
    const QJsonValue data = o.value(QLatin1String("data"));
    if (data.isArray()) {
      QByteArray bytes;
      for (const QJsonValue &b : data.toArray()) bytes.append(char(json::unsignedField(b, "data", 255)));
      file.data = bytes;
    }
    files.push_back(file);
  }
  return files;
}

QJsonObject ImportResult::toJson() const {
  QJsonArray addedJson, failedJson;
  for (const DetachedMediaEntry &entry : added) addedJson.append(entry.toJson());
  for (const auto &[name, error] : failed) failedJson.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("error"), error}});
  return {{QStringLiteral("added"), addedJson}, {QStringLiteral("failed"), failedJson}};
}

QJsonArray getAlbumsDetached(const QString &root) {
  if (!isDir(root)) fail(QStringLiteral("%1 is not a directory").arg(root));
  if (catalog::nestedInVault(root)) return {};
  const bool initialInventory = preload::setActiveRoot(root);
  const std::vector<AlbumDirEntry> entries = walkAlbumEntries(root);
  catalog::initialize(root);
  std::vector<std::pair<int, std::optional<QString>>> summaries;
  for (const AlbumDirEntry &entry : entries) {
    const int count = catalog::scanAlbumIfNeeded(root, entry.path, initialInventory);
    // Existing covers only: startup inventory never decodes originals.
    const QString thumbs = path::join(entry.path, thumb::kThumbDir);
    std::optional<QString> preview;
    if (isDir(thumbs)) {
      try {
        for (const DirEntry &file : readDir(entry.path)) {
          if (!file.isFile || !constants::isMedia(file.name)) continue;
          const QString candidate = thumb::thumbPath(path::join(entry.path, file.name), thumbs);
          if (isFile(candidate)) {
            preview = candidate;
            break;
          }
        }
      } catch (const Error &) {
      }
    }
    summaries.emplace_back(count, preview);
  }
  QJsonArray albums;
  for (size_t i = 0; i < entries.size(); ++i) {
    const AlbumDirEntry &entry = entries[i];
    std::optional<QString> cover = summaries[i].second;
    if (summaries[i].first <= 0) {
      cover.reset();
      const QString prefix = entry.relativePath + QLatin1Char('/');
      for (size_t j = i + 1; j < entries.size() && entries[j].relativePath.startsWith(prefix); ++j)
        if (summaries[j].second) {
          cover = summaries[j].second;
          break;
        }
    }
    DetachedAlbum album;
    album.path = entry.path;
    album.name = entry.name;
    album.thumbPath = cover;
    album.size = summaries[i].first;
    album.relativePath = entry.relativePath;
    album.parent = entry.parent;
    try {
      album.directoryId = directoryId(entry.path);
    } catch (const Error &) {
    }
    albums.append(album.toJson());
    preload::enqueuePreload(entry.path);
  }
  QStringList ids;
  for (const AlbumDirEntry &entry : entries) ids << entry.relativePath;
  catalog::retainAlbums(root, ids);
  preload::startPreloaderWorker();
  return albums;
}

QJsonArray getAlbumMedia(const QString &dir) {
  // Compatibility endpoint is also bounded. New clients use get_media_page.
  std::optional<QString> root = catalog::libraryRoot(dir);
  if (!root) {
    const QString parent = path::parent(dir);
    if (parent.isEmpty()) fail(QStringLiteral("Missing library root"));
    root = parent;
  }
  catalog::initialize(*root);
  catalog::scanAlbum(*root, dir);
  catalog::PageQuery query;
  query.rootDir = *root;
  query.albumId = catalog::albumId(*root, dir);
  query.sortKey = QStringLiteral("name");
  query.sortDir = QStringLiteral("asc");
  query.randomSeed = 1.0;
  QJsonArray items;
  for (const catalog::PageItem &item : catalog::queryPage(query).items) items.append(item.media.toJson());
  return items;
}

QJsonArray listFavorites(const QString &rootDir) {
  catalog::PageQuery query;
  query.rootDir = rootDir;
  query.favoritesOnly = true;
  query.sortKey = QStringLiteral("name");
  query.sortDir = QStringLiteral("asc");
  query.randomSeed = 1.0;
  QJsonArray out;
  for (const catalog::PageItem &item : catalog::queryPage(query).items) {
    QJsonObject o{{QStringLiteral("revision"), item.media.revision}, {QStringLiteral("meta"), item.media.meta}, {QStringLiteral("name"), item.media.name}, {QStringLiteral("albumPath"), item.albumPath}, {QStringLiteral("albumName"), item.albumName}, {QStringLiteral("albumId"), item.albumId}, {QStringLiteral("favorite"), true}};
    if (item.media.thumbnailName) o.insert(QStringLiteral("thumbnailName"), *item.media.thumbnailName);
    out.append(o);
  }
  return out;
}

void deleteAlbum(const QString &rootDir, const QString &albumId) {
  QStringList parts;
  bool valid = !albumId.trimmed().isEmpty() && !path::isAbsolute(albumId) && !albumId.startsWith(QLatin1Char('/'));
#ifdef Q_OS_WIN
  const QStringList raw = albumId.split(QRegularExpression(QStringLiteral("[/\\\\]")));
#else
  const QStringList raw = albumId.split(QLatin1Char('/'));
#endif
  for (int i = 0; i < raw.size() && valid; ++i) {
    const QString &part = raw[i];
    if (part.isEmpty()) continue;
    if (part == QLatin1String(".")) {
      if (parts.isEmpty() && i == 0) valid = false;
      continue;
    }
    if (part == QLatin1String("..") || part.startsWith(QLatin1String(".room237-")) || part.contains(QLatin1Char(':'))) valid = false;
    else parts << part;
  }
  if (!valid || parts.isEmpty()) fail(QStringLiteral("A non-root album id is required"));
  const QString requested = path::join(rootDir, parts.join(kSeparator));
  const QString root = canonical(rootDir);
  QString target = root;
  for (const QString &part : parts) {
    target = path::join(target, part);
    const FileInfo info = lstat(target);
    if (!info.exists) fail(QStringLiteral("No such file or directory (os error 2)"));
    if (info.isSymlink) fail(QStringLiteral("Cannot delete a symbolic-link album"));
  }
  target = canonical(target);
  if (target == root || !path::startsWith(target, root) || !isDir(target)) fail(QStringLiteral("Album path escapes the library or is not a directory"));
  const QString identity = directoryId(target);
  // Stop queued work before waiting for current readers; otherwise workers can keep reacquiring shared leases for
  // hundreds of queued thumbnails.
  preload::dropPreloadForPath(requested);
  preload::dropPreloadForPath(target);
  auto guard = prepareAlbumTreeRename(root, target);
  if (directoryId(target) != identity) fail(QStringLiteral("Album changed while waiting for deletion"));
  preload::dropPreloadForPath(requested);
  preload::dropPreloadForPath(target);
  removeTree(target);
}

static RenamedAlbumResult renamed(const QString &root, const QString &oldPath, const QString &newPath, const QString &parentDir, const QString &name) {
  RenamedAlbumResult result;
  result.oldPath = oldPath;
  result.newPath = newPath;
  result.oldRelativePath = normalized(path::relative(oldPath, root));
  result.newRelativePath = normalized(path::relative(newPath, root));
  if (parentDir != root) result.parent = normalized(path::relative(parentDir, root));
  result.name = name;
  return result;
}

RenamedAlbumResult renameAlbum(const QString &rootDir, const QString &albumId, const QString &newName) {
  if (albumId.trimmed().isEmpty()) fail(QStringLiteral("Album id is required"));
  if (albumId.compare(QLatin1String("favorites"), Qt::CaseInsensitive) == 0) fail(QStringLiteral("Cannot rename favorites album"));
  const QString trimmed = newName.trimmed();
  if (trimmed.isEmpty()) fail(QStringLiteral("Album name cannot be empty"));
  QString safe = trimmed;
  safe.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_')).replace(QLatin1Char(':'), QLatin1Char('_'));
  validateFileName(safe);
  if (safe.startsWith(QLatin1String(".room237-"))) fail(QStringLiteral("Reserved album name"));
  const quint64 cleanupDelay = readSettings().album.renameCleanupDelaySecs;
  if (!isDir(rootDir)) fail(QStringLiteral("%1 is not a directory").arg(rootDir));
  QString root;
  try {
    root = canonical(rootDir);
  } catch (const Error &e) {
    fail(QStringLiteral("Failed to resolve root: %1").arg(e.message()));
  }
  const auto target = tryCanonical(path::join(root, albumId));
  if (!target) fail(QStringLiteral("Album not found"));
  if (*target == root) fail(QStringLiteral("Cannot rename or move the root directory"));
  if (!path::startsWith(*target, root)) fail(QStringLiteral("Album path escapes root"));
  const QString parentDir = path::parent(*target);
  if (parentDir.isEmpty()) fail(QStringLiteral("Cannot rename root directory"));
  if (path::fileName(*target) == safe) return renamed(root, *target, *target, parentDir, safe);
  const QString newPath = path::join(parentDir, safe);
  if (exists(newPath)) fail(QStringLiteral("Album \"%1\" already exists in %2").arg(safe, parentDir));
  {
    auto moving = prepareAlbumTreeRename(root, *target);
    QString error;
    if (!renameNoClobber(*target, newPath, &error)) fail(error);
  }
  preload::dropPreloadForPath(*target);
  if (cleanupDelay > 0) {
    const QString old = *target;
    std::thread([old, newPath, cleanupDelay] {
      std::this_thread::sleep_for(std::chrono::seconds(cleanupDelay));
      try {
        if (old != newPath && exists(old) && listDir(old).isEmpty()) QDir().rmdir(old);
      } catch (const Error &) {
      }
    }).detach();
  }
  return renamed(root, *target, newPath, parentDir, safe);
}

RenamedAlbumResult moveAlbum(const QString &rootDir, const QString &albumId, const std::optional<QString> &newParentId) {
  if (albumId.trimmed().isEmpty()) fail(QStringLiteral("Album id is required"));
  if (albumId.compare(QLatin1String("favorites"), Qt::CaseInsensitive) == 0) fail(QStringLiteral("Cannot move favorites album"));
  if (!isDir(rootDir)) fail(QStringLiteral("%1 is not a directory").arg(rootDir));
  QString root;
  try {
    root = canonical(rootDir);
  } catch (const Error &e) {
    fail(QStringLiteral("Failed to resolve root: %1").arg(e.message()));
  }
  const auto target = tryCanonical(path::join(root, albumId));
  if (!target) fail(QStringLiteral("Album not found"));
  if (*target == root) fail(QStringLiteral("Cannot rename or move the root directory"));
  if (!path::startsWith(*target, root)) fail(QStringLiteral("Album path escapes root"));
  const QString name = path::fileName(*target);
  if (name.isEmpty()) fail(QStringLiteral("Invalid album name"));
  if (path::parent(*target).isEmpty()) fail(QStringLiteral("Cannot move root directory"));
  const QString wanted = newParentId && !newParentId->trimmed().isEmpty() ? path::join(root, *newParentId) : root;
  const auto parent = tryCanonical(wanted);
  if (!parent) fail(QStringLiteral("Target album not found"));
  if (!isDir(*parent)) fail(QStringLiteral("Target album is not a directory"));
  if (!path::startsWith(*parent, root)) fail(QStringLiteral("Target path escapes root"));
  if (path::startsWith(*parent, *target)) fail(QStringLiteral("Cannot move album into its own subtree"));
  const QString newPath = path::join(*parent, name);
  if (newPath == *target) return renamed(root, *target, *target, *parent, name);
  if (exists(newPath)) fail(QStringLiteral("Album \"%1\" already exists in %2").arg(name, *parent));
  {
    auto moving = prepareAlbumTreeRename(root, *target);
    QString error;
    if (!renameNoClobber(*target, newPath, &error)) fail(error);
  }
  preload::dropPreloadForPath(*target);
  return renamed(root, *target, newPath, *parent, name);
}

quint64 getAlbumSize(const QString &dir) {
  if (!isDir(dir)) fail(QStringLiteral("%1 is not a directory").arg(dir));
  media_delete::recoverAlbum(dir);
  quint64 total = 0;
  for (const DirEntry &entry : readDir(dir)) {
    const QString p = path::join(dir, entry.name);
    const FileInfo info = stat(p);
    if (info.isFile && constants::isMedia(p)) total += info.size;
  }
  return total;
}

QStringList moveMediaBatchWithHook(const QString &sourceRequested, const QString &targetRequested, const QStringList &media, bool moveArtifacts, const std::function<void(const QString &, const QString &, const QString &)> &checkpoint) {
  const QString sourceDir = canonical(sourceRequested), targetDir = canonical(targetRequested);
  if (!isDir(sourceDir) || !isDir(targetDir)) fail(QStringLiteral("bad dirs"));
  for (const QString &name : media) validateFileName(name);
  if (sourceDir == targetDir) return {};
  auto tree = album_tree::read({sourceDir, targetDir});
  media_delete::recoverAlbum(sourceDir);
  media_delete::recoverAlbum(targetDir);
  // Use the same canonical order as deletion. Freeze encoders before album leases.
  QStringList directories{sourceDir, targetDir};
  std::sort(directories.begin(), directories.end());
  std::vector<std::unique_ptr<FileLock>> maintenance;
  for (const QString &dir : directories) maintenance.push_back(media_delete::lockMaintenance(dir));
  const AlbumMeta observed = moveArtifacts ? AlbumMeta() : metadata::readSelectedMetadata(sourceDir, media, true);
  const bool targetHasMetadata = !moveArtifacts && exists(path::join(targetDir, metadata::kMetaDir));
  bool hasPair = false;
  if (!moveArtifacts)
    for (const MoveGroup &group : logicalMoveGroups(sourceDir, media, observed)) hasPair |= group.size() > 1;
  std::unique_lock<std::mutex> moving, store;
  std::optional<metadata::MetadataWriteGuard> metadataGuard;
  if (moveArtifacts || hasPair || targetHasMetadata) {
    auto locked = metadata::lockMetadataForMoveWithTree({sourceDir, targetDir}, *tree);
    moving = std::move(locked.first);
    metadataGuard = std::move(locked.second);
  } else {
    tree->ensureCurrent({sourceDir, targetDir});
    moving = std::unique_lock(mediaMoveLock);
    store = std::unique_lock(storeWriteLock);
  }
  AlbumMeta snapshot = observed;
  if (metadataGuard) {
    metadata::readSelectedWithGuard(targetDir, media, false, *metadataGuard);
    snapshot = metadata::readSelectedWithGuard(sourceDir, media, true, *metadataGuard);
  }
  const QHash<QString, MoveGroup> groups = logicalMoveGroups(sourceDir, media, snapshot);
  QStringList failed;
  std::vector<PlannedMove> movedRecords;
  std::unique_ptr<media_move::MoveJournal> journal;
  QHash<QString, std::vector<PlannedMove>> initialPlans;
  std::vector<PlannedMove> initialRecords;
  QSet<QString> planningAttempted, reserved;
  for (const QString &requested : media) {
    const MoveGroup &group = groups[requested];
    const QString key = group[0].first;
    if (planningAttempted.contains(key)) continue;
    planningAttempted.insert(key);
    try {
      const std::vector<PlannedMove> plan = planMoveGroup(sourceDir, targetDir, group, reserved);
      initialRecords.insert(initialRecords.end(), plan.begin(), plan.end());
      initialPlans.insert(key, plan);
    } catch (const Error &) {
    }
  }
  auto records = [](const std::vector<PlannedMove> &plan) {
    std::vector<media_move::FileRecord> out;
    for (const PlannedMove &move : plan) out.push_back({move.source, move.target, move.revision});
    return out;
  };
  if (!initialRecords.empty()) {
    journal = media_move::MoveJournal::create(sourceDir, targetDir);
    journal->recordFilesWithArtifacts(records(initialRecords), moveArtifacts);
  }
  QSet<QString> processed;
  for (const QString &requested : media) {
    if (processed.contains(requested)) continue;
    const MoveGroup &group = groups[requested];
    std::vector<PlannedMove> groupRecords;
    try {
      QSet<QString> none;
      const std::vector<PlannedMove> planned = planMoveGroup(sourceDir, targetDir, group, none);
      if (!journal) journal = media_move::MoveJournal::create(sourceDir, targetDir);
      if (!initialPlans.contains(group[0].first) || !(initialPlans.value(group[0].first) == planned)) journal->recordFilesWithArtifacts(records(planned), moveArtifacts);
      for (const PlannedMove &move : planned) {
        const QString source = path::join(sourceDir, move.source), destination = path::join(targetDir, move.target);
        checkpoint(QStringLiteral("before_rename"), source, destination);
        if (metadata::sourceRevision(source) != move.revision) fail(QStringLiteral("Move source changed"));
        QString error;
        if (!renameNoClobber(source, destination, &error)) fail(error);
        groupRecords.push_back(move);
        if (metadata::sourceRevision(destination) != move.revision) fail(QStringLiteral("Moved file changed"));
      }
    } catch (const Error &error) {
      try {
        rollbackMovedFiles(sourceDir, targetDir, groupRecords);
      } catch (const Error &rollback) {
        QString previous = QStringLiteral("Ok(())");
        try {
          rollbackMovedFiles(sourceDir, targetDir, movedRecords);
        } catch (const Error &e) {
          previous = QStringLiteral("Err(%1)").arg(json::string(e.message()));
        }
        fail(QStringLiteral("%1; group rollback: %2; previous rollback: %3").arg(error.message(), rollback.message(), previous));
      }
      qWarning("move failed %s: %s", qUtf8Printable(requested), qUtf8Printable(error.message()));
      failed << requested;
      processed.insert(requested);
      continue;
    }
    failed.erase(std::remove_if(failed.begin(), failed.end(), [&](const QString &name) { return std::any_of(group.begin(), group.end(), [&](const auto &member) { return member.first == name; }); }), failed.end());
    for (const auto &member : group) processed.insert(member.first);
    movedRecords.insert(movedRecords.end(), groupRecords.begin(), groupRecords.end());
  }
  std::vector<std::pair<QString, QString>> moved;
  for (const PlannedMove &move : movedRecords) moved.emplace_back(move.source, move.target);
  if (metadataGuard) {
    try {
      checkpoint(QStringLiteral("before_metadata"), sourceDir, targetDir);
      for (const PlannedMove &move : movedRecords)
        if (metadata::sourceRevision(path::join(targetDir, move.target)) != move.revision) fail(QStringLiteral("Moved file changed before metadata transfer"));
      if (!moved.empty()) {
        if (!journal) fail(QStringLiteral("Missing move journal"));
        metadata::transferMediaState(sourceDir, targetDir, moved, *metadataGuard, moveArtifacts, [&](const metadata::MoveMetadataUndo &undo) { journal->recordMetadata(undo); }, [&](const QString &phase) { checkpoint(phase, sourceDir, targetDir); });
      }
    } catch (const Error &error) {
      if (journal) {
        try {
          journal->rollback(&*metadataGuard);
        } catch (const Error &rollback) {
          fail(QStringLiteral("%1; file rollback: %2").arg(error.message(), rollback.message()));
        }
      }
      throw;
    }
  }
  checkpoint(QStringLiteral("before_move_commit"), sourceDir, targetDir);
  if (journal) journal->commit([&] { checkpoint(QStringLiteral("move_committed"), sourceDir, targetDir); });
  checkpoint(QStringLiteral("after_move_commit"), sourceDir, targetDir);
  if (journal) {
    try {
      journal->finish([](const QString &) {});
    } catch (const Error &error) {
      // Original data is already committed. Derived work remains best-effort, with the journal retained for the next
      // recovery.
      qWarning("Failed to finish move artifacts: %s", qUtf8Printable(error.message()));
    }
  }
  return failed;
}

QStringList moveMediaBatch(const QString &source, const QString &target, const QStringList &media) {
  return moveMediaBatchWithHook(source, target, media, readSettings().album.moveRenameThumbsAndMeta, [](const QString &, const QString &, const QString &) {});
}

QString moveMedia(const QString &source, const QString &target, const QString &media) {
  if (!moveMediaBatch(source, target, {media}).isEmpty()) fail(QStringLiteral("Failed to move media"));
  return QStringLiteral("ok");
}

DetachedMediaEntry registerNewMedia(const QString &albumPath, const QString &mediaName) {
  validateFileName(mediaName);
  const QString file = path::join(albumPath, mediaName);
  if (!isFile(file)) fail(QStringLiteral("Changed media is not a file"));
  metadata::invalidateMediaContent(file);
  ImportResult result = addMediaFilesBlocking(albumPath, {IncomingFile{mediaName, file, std::nullopt}});
  if (!result.added.empty()) return result.added.back();
  fail(result.failed.empty() ? QStringLiteral("No media registered") : result.failed.front().second);
}

ImportResult addMediaFiles(const QString &dir, const std::vector<IncomingFile> &files) { return addMediaFilesBlocking(dir, files); }

}  // namespace core::album
