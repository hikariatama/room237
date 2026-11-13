#include "media_move.h"

#include "album_tree.h"
#include "json.h"
#include "media_delete.h"
#include "thumb.h"
#include "util.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <set>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace core::media_move {

namespace {
const QString kPrefix = QStringLiteral(".room237-move-");
const QString kHeader = QStringLiteral("header.json");
const QString kEvents = QStringLiteral("events.jsonl");
constexpr qint64 kHeaderLimit = 16 * 1024;
constexpr qint64 kRecordLimit = 16 * 1024 * 1024;
constexpr qint64 kJournalLimit = 128 * 1024 * 1024;

enum class ArtifactKind { Preview, LegacyMetadata };
QString kindName(ArtifactKind kind) { return kind == ArtifactKind::Preview ? QStringLiteral("Preview") : QStringLiteral("LegacyMetadata"); }
QString kindDirectory(ArtifactKind kind) { return kind == ArtifactKind::Preview ? thumb::kThumbDir : metadata::kMetaDir; }
QString kindPath(ArtifactKind kind, const QString &album, const QString &name) {
  const QString dir = path::join(album, kindDirectory(kind));
  if (kind == ArtifactKind::Preview) return thumb::thumbPath(path::join(album, name), dir);
  return path::join(dir, name + QStringLiteral(".meta"));
}
bool kindValidName(ArtifactKind kind, const QString &media, const QString &file) {
  if (kind == ArtifactKind::Preview) {
    for (const QString &candidate : thumb::deletedThumbnailPaths(media, QString()))
      if (path::fileName(candidate) == file) return true;
    return false;
  }
  return file == media + QStringLiteral(".meta");
}

struct Artifact {
  ArtifactKind kind = ArtifactKind::Preview;
  QString source, target, mediaRevision, sourceFile, targetFile, revision, sourceDirectoryId;
  void validate() const {
    for (const QString *name : {&source, &target, &sourceFile, &targetFile}) validateFileName(*name);
    if (mediaRevision.isEmpty() || revision.isEmpty() || sourceDirectoryId.isEmpty() || !kindValidName(kind, source, sourceFile) || !kindValidName(kind, target, targetFile)) fail(QStringLiteral("Invalid move artifact"));
  }
  QString toJson() const {
    return json::Object().str("kind", kindName(kind)).str("source", source).str("target", target).str("media_revision", mediaRevision).str("source_file", sourceFile).str("target_file", targetFile).str("revision", revision).str("source_directory_id", sourceDirectoryId).compact();
  }
};

struct Event {
  enum class Type { File, Artifact, Metadata, Committed, RolledBack } type = Type::Committed;
  FileRecord file;
  Artifact artifact;
  metadata::MoveMetadataUndo undo;
  bool settles() const { return type == Type::Committed || type == Type::RolledBack; }
};

QString fileEventJson(const FileRecord &record) { return json::Object().raw("File", json::Object().str("source", record.source).str("target", record.target).str("revision", record.revision).compact()).compact(); }

Event parseEvent(const QByteArray &line) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson("[" + line + "]", &error);
  if (error.error != QJsonParseError::NoError) fail(QStringLiteral("Invalid move journal: %1").arg(error.errorString()));
  const QJsonValue value = document.array().first();
  Event event;
  if (value.isString()) {
    if (value.toString() == QLatin1String("Committed")) event.type = Event::Type::Committed;
    else if (value.toString() == QLatin1String("RolledBack")) event.type = Event::Type::RolledBack;
    else fail(QStringLiteral("Invalid move journal: unknown variant"));
    return event;
  }
  const QJsonObject object = value.toObject();
  if (object.size() != 1) fail(QStringLiteral("Invalid move journal: expected an event"));
  const QString tag = object.begin().key();
  const QJsonObject body = object.begin().value().toObject();
  try {
    if (tag == QLatin1String("File")) {
      event.type = Event::Type::File;
      event.file = {json::stringField(body.value(QLatin1String("source")), "source"), json::stringField(body.value(QLatin1String("target")), "target"), json::stringField(body.value(QLatin1String("revision")), "revision")};
    } else if (tag == QLatin1String("Artifact")) {
      event.type = Event::Type::Artifact;
      const QString kind = json::stringField(body.value(QLatin1String("kind")), "kind");
      if (kind == QLatin1String("Preview")) event.artifact.kind = ArtifactKind::Preview;
      else if (kind == QLatin1String("LegacyMetadata")) event.artifact.kind = ArtifactKind::LegacyMetadata;
      else fail(QStringLiteral("unknown variant `%1`").arg(kind));
      event.artifact.source = json::stringField(body.value(QLatin1String("source")), "source");
      event.artifact.target = json::stringField(body.value(QLatin1String("target")), "target");
      event.artifact.mediaRevision = json::stringField(body.value(QLatin1String("media_revision")), "media_revision");
      event.artifact.sourceFile = json::stringField(body.value(QLatin1String("source_file")), "source_file");
      event.artifact.targetFile = json::stringField(body.value(QLatin1String("target_file")), "target_file");
      event.artifact.revision = json::stringField(body.value(QLatin1String("revision")), "revision");
      event.artifact.sourceDirectoryId = json::stringField(body.value(QLatin1String("source_directory_id")), "source_directory_id");
    } else if (tag == QLatin1String("Metadata")) {
      event.type = Event::Type::Metadata;
      event.undo = metadata::MoveMetadataUndo::fromJson(body);
    } else {
      fail(QStringLiteral("unknown variant `%1`").arg(tag));
    }
  } catch (const Error &e) {
    fail(QStringLiteral("Invalid move journal: %1").arg(e.message()));
  }
  return event;
}

std::optional<QString> regularRevision(const QString &p) {
  const FileInfo info = lstat(p);
  if (!info.isFile) return std::nullopt;
  return metadata::sourceRevisionFromInfo(info);
}

void syncDirectory(const QString &p) {
#ifndef Q_OS_WIN
  syncDir(p);
#else
  Q_UNUSED(p)
#endif
}

void syncAlbums(const Header &header) {
  for (const QString *album : {&header.source, &header.target}) {
    // Metadata files are synced at atomic publication; their directory renames and the original-media renames must
    // precede the commit record.
    const QString meta = path::join(*album, metadata::kMetaDir);
    if (lstat(meta).isDir) syncDirectory(meta);
    syncDirectory(*album);
  }
}

QByteArray readRegular(const QString &p, qint64 limit) {
  if (!lstat(p).isFile) fail(QStringLiteral("Move journal is not a regular file: %1").arg(p));
  return readAll(p, limit + 1);
}

QString headerJson(const Header &header) { return json::Object().num("version", header.version).str("source", header.source).str("target", header.target).str("source_id", header.sourceId).str("target_id", header.targetId).compact(); }

Header readHeader(const QString &staging) {
  if (!lstat(staging).isDir) fail(QStringLiteral("Move staging is not a regular directory"));
  const QByteArray bytes = readRegular(path::join(staging, kHeader), kHeaderLimit);
  if (bytes.size() > kHeaderLimit) fail(QStringLiteral("Move header too large"));
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) fail(QStringLiteral("Invalid move header: %1").arg(error.errorString()));
  const QJsonObject o = document.object();
  try {
    Header header;
    header.version = int(json::unsignedField(o.value(QLatin1String("version")), "version", 255));
    header.source = json::stringField(o.value(QLatin1String("source")), "source");
    header.target = json::stringField(o.value(QLatin1String("target")), "target");
    header.sourceId = json::stringField(o.value(QLatin1String("source_id")), "source_id");
    header.targetId = json::stringField(o.value(QLatin1String("target_id")), "target_id");
    return header;
  } catch (const Error &e) {
    fail(QStringLiteral("Invalid move header: %1").arg(e.message()));
  }
}

std::pair<std::vector<Event>, qint64> readEvents(const QString &staging) {
  const QString p = path::join(staging, kEvents);
  if (!lstat(p).exists) return {{}, 0};
  if (!lstat(p).isFile) fail(QStringLiteral("Move journal is not a regular file: %1").arg(p));
  if (qint64(lstat(p).size) > kJournalLimit) fail(QStringLiteral("Move journal is too large"));
  const QByteArray bytes = readAll(p);
  std::vector<Event> events;
  qint64 valid = 0, at = 0;
  while (at < bytes.size()) {
    const qsizetype end = bytes.indexOf('\n', at);
    const qint64 length = (end < 0 ? bytes.size() : end + 1) - at;
    if (length > kRecordLimit) fail(QStringLiteral("Move journal record too large"));
    // A process can die during append. That incomplete final record was never synced, so the operations it describes
    // were not started.
    if (end < 0) break;
    const Event event = parseEvent(bytes.mid(at, end - at));
    switch (event.type) {
    case Event::Type::File:
      validateFileName(event.file.source);
      validateFileName(event.file.target);
      if (event.file.revision.isEmpty()) fail(QStringLiteral("Empty move revision"));
      break;
    case Event::Type::Metadata: event.undo.validate(); break;
    case Event::Type::Artifact: event.artifact.validate(); break;
    default: break;
    }
    if (!events.empty() && events.back().settles()) fail(QStringLiteral("Events after move commit"));
    valid += length;
    at += length;
    events.push_back(event);
  }
  return {events, valid};
}

void removeKnownDirectory(const QString &p, std::initializer_list<QString> names) {
  if (!exists(p)) return;
  if (!lstat(p).isDir) fail(QStringLiteral("Move staging changed; preserving it"));
  for (const DirEntry &entry : readDir(p)) {
    bool known = false;
    for (const QString &name : names) known |= entry.name == name;
    if (!known || !entry.isFile) fail(QStringLiteral("Unknown files in move journal; preserving them"));
  }
  for (const QString &name : names) removeFileIfExists(path::join(p, name));
  const QString parent = path::parent(p);
#ifndef Q_OS_WIN
  if (::rmdir(QFile::encodeName(p).constData()) != 0) fail(QStringLiteral("Cannot remove %1").arg(p));
#else
  if (!RemoveDirectoryW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(p).utf16()))) fail(QStringLiteral("Cannot remove %1").arg(p));
#endif
  if (parent.isEmpty()) fail(QStringLiteral("Missing journal parent"));
  syncDirectory(parent);
}

void cleanup(const QString &staging, const QString &peer) {
  // Remove the locator first, retaining the authoritative commit record until there is no peer that could request
  // recovery through it.
  removeKnownDirectory(peer, {kHeader});
  removeKnownDirectory(staging, {kEvents, kHeader});
}

void quarantineIncomplete(const QString &p) {
  const QString name = path::fileName(p);
  if (name.isEmpty()) fail(QStringLiteral("Missing journal name"));
  const QString quarantine = path::join(path::parent(p), QStringLiteral(".room237-unfinished-") + name);
  QString error;
  if (!renameNoClobber(p, quarantine, &error)) fail(error);
  syncDirectory(path::parent(p));
  qWarning("Preserved incomplete move staging in %s", qUtf8Printable(quarantine));
}

void finishArtifacts(const Header &header, const std::vector<Event> &events, const std::function<void(const QString &)> &checkpoint) {
  std::set<std::pair<int, QString>> seen;
  // A retry can pick another collision suffix; only its latest intent counts.
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->type != Event::Type::Artifact) continue;
    const Artifact &artifact = it->artifact;
    if (!seen.insert({int(artifact.kind), artifact.source}).second) continue;
    // Initial plans include failed groups. Only committed original renames authorize their derived artifacts.
    if (lstat(path::join(header.source, artifact.source)).exists || regularRevision(path::join(header.target, artifact.target)) != artifact.mediaRevision) continue;
    const QString sourceDir = path::join(header.source, kindDirectory(artifact.kind));
    try {
      if (directoryId(sourceDir) != artifact.sourceDirectoryId) continue;
    } catch (const Error &) {
      continue;
    }
    const QString source = path::join(sourceDir, artifact.sourceFile);
    const QString targetDir = path::join(header.target, kindDirectory(artifact.kind));
    const QString target = path::join(targetDir, artifact.targetFile);
    // Absence also covers a previous recovery interrupted after this rename.
    if (regularRevision(source) != artifact.revision) continue;
    try {
      createDir(targetDir);
      syncDirectory(header.target);
    } catch (const Error &) {
      if (!entryExists(targetDir)) throw;
    }
    try {
      directoryId(targetDir);
    } catch (const Error &) {
      qWarning("Preserving move artifact: target directory changed");
      continue;
    }
    QString error;
    if (renameNoClobber(source, target, &error)) checkpoint(QStringLiteral("move_artifact_renamed"));
    else if (entryExists(target)) qWarning("Preserving conflicting move artifact %s", qUtf8Printable(source));
    else fail(error);
  }
  // Also sync after resuming an already-performed rename, before removing the only remaining journal that knows it
  // needs durable directory entries.
  for (const QString *album : {&header.source, &header.target}) {
    for (ArtifactKind kind : {ArtifactKind::Preview, ArtifactKind::LegacyMetadata}) {
      const QString dir = path::join(*album, kindDirectory(kind));
      try {
        directoryId(dir);
        syncDirectory(dir);
      } catch (const Error &) {
      }
    }
    syncDirectory(*album);
  }
}

bool sameRegular(const QString &p, const QString &revision) {
  const FileInfo info = lstat(p);
  return info.isFile && metadata::sourceRevisionFromInfo(info) == revision;
}

void rollbackFiles(const Header &header, const std::vector<Event> &events) {
  // A failed group can be retried through its other pair member. Its latest intent supersedes earlier destinations
  // that were already rolled back.
  QSet<QString> seen;
  std::vector<FileRecord> files;
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->type != Event::Type::File || seen.contains(it->file.source)) continue;
    seen.insert(it->file.source);
    files.push_back(it->file);
  }
  QStringList errors;
  for (const FileRecord &file : files) {
    const QString original = path::join(header.source, file.source), moved = path::join(header.target, file.target);
    if (sameRegular(original, file.revision)) continue;
    if (lstat(original).exists || !sameRegular(moved, file.revision)) errors << QStringLiteral("Move originals changed; preserving %1 and %2").arg(file.source, file.target);
  }
  if (!errors.isEmpty()) fail(errors.join(QStringLiteral("; ")));
  for (const FileRecord &file : files) {
    const QString original = path::join(header.source, file.source), moved = path::join(header.target, file.target);
    if (sameRegular(original, file.revision)) continue;
    if (!sameRegular(moved, file.revision)) {
      errors << QStringLiteral("Moved original changed or missing; preserving %1").arg(file.target);
      continue;
    }
    QString error;
    if (!renameNoClobber(moved, original, &error)) errors << QStringLiteral("Cannot restore %1: %2").arg(file.source, error);
  }
  syncDirectory(header.source);
  syncDirectory(header.target);
  if (!errors.isEmpty()) fail(errors.join(QStringLiteral("; ")));
}

void validateUndo(const Header &header, const std::vector<Event> &events, const metadata::MetadataWriteGuard *guard) {
  for (const Event &event : events) {
    if (event.type != Event::Type::Metadata) continue;
    if (!guard) fail(QStringLiteral("Missing metadata lease"));
    metadata::validateMoveMetadataUndoWithGuard(header.source, header.target, event.undo, *guard);
  }
}

void syncHandle(QFile &file) {
  file.flush();
#ifdef Q_OS_WIN
  if (!FlushFileBuffers(HANDLE(_get_osfhandle(file.handle())))) fail(QStringLiteral("Cannot sync move journal"));
#elif defined(Q_OS_MACOS)
  if (::fcntl(file.handle(), F_FULLFSYNC) != 0 && ::fsync(file.handle()) != 0) fail(QStringLiteral("Cannot sync move journal"));
#else
  if (::fsync(file.handle()) != 0) fail(QStringLiteral("Cannot sync move journal"));
#endif
}
}  // namespace

void Header::validate(const QString &album) const {
  if (version != 1 || source == target || (album != source && album != target) || tryCanonical(source) != source || tryCanonical(target) != target || directoryId(source) != sourceId || directoryId(target) != targetId)
    fail(QStringLiteral("Move album identity changed; preserving journal"));
}

std::unique_ptr<MoveJournal> MoveJournal::create(const QString &source, const QString &target) {
  auto journal = std::unique_ptr<MoveJournal>(new MoveJournal);
  journal->header_ = {1, source, target, directoryId(source), directoryId(target)};
  const QByteArray bytes = headerJson(journal->header_).toUtf8();
  if (bytes.size() > kHeaderLimit) fail(QStringLiteral("Move header too large"));
  TempDir staging(source, kPrefix);
  journal->staging_ = staging.path();
  staging.release();
  journal->peer_ = path::join(target, path::fileName(journal->staging_));
  // The peer is a locator, not another independently committed journal.
  atomicWriteExisting(path::join(journal->staging_, kHeader), bytes);
  const QString events = path::join(journal->staging_, kEvents);
  journal->events_ = std::make_unique<QFile>(events);
  if (!journal->events_->open(QIODevice::ReadWrite | QIODevice::NewOnly | QIODevice::Unbuffered)) fail(journal->events_->errorString());
  createDir(journal->peer_);
  atomicWriteExisting(path::join(journal->peer_, kHeader), bytes);
  for (const QString &dir : {journal->staging_, journal->peer_, source, target}) syncDirectory(dir);
  return journal;
}

void MoveJournal::append(const QString &json, bool terminal) {
  if (failed_) fail(QStringLiteral("Move journal write failed"));
  const QByteArray bytes = json.toUtf8() + '\n';
  const qint64 current = events_->size();
  const qint64 limit = terminal ? kJournalLimit : kJournalLimit - 32;
  if (bytes.size() > kRecordLimit || current + bytes.size() > limit) fail(QStringLiteral("Move journal is too large"));
  events_->seek(current);
  if (events_->write(bytes) != bytes.size()) {
    failed_ = true;
    events_->resize(current);
    events_->seek(current);
    fail(events_->errorString());
  }
}

void MoveJournal::sync() {
  try {
    syncHandle(*events_);
  } catch (const Error &) {
    failed_ = true;
    throw;
  }
}

void MoveJournal::recordFilesWithArtifacts(const std::vector<FileRecord> &files, bool moveArtifacts) {
  for (const FileRecord &file : files) {
    append(fileEventJson(file), false);
    if (!moveArtifacts) continue;
    for (ArtifactKind kind : {ArtifactKind::Preview, ArtifactKind::LegacyMetadata}) {
      const QString sourceDir = path::join(header_.source, kindDirectory(kind));
      // Never follow a derived directory or file outside the album.
      QString sourceDirectoryId;
      try {
        sourceDirectoryId = directoryId(sourceDir);
      } catch (const Error &) {
        continue;
      }
      const QString artifactPath = kindPath(kind, header_.source, file.source);
      const auto artifactRevision = regularRevision(artifactPath);
      if (!artifactRevision) continue;
      const QString destination = kindPath(kind, header_.target, file.target);
      Artifact artifact{kind, file.source, file.target, file.revision, path::fileName(artifactPath), path::fileName(destination), *artifactRevision, sourceDirectoryId};
      append(json::Object().raw("Artifact", artifact.toJson()).compact(), false);
    }
  }
  sync();
}

void MoveJournal::recordMetadata(const metadata::MoveMetadataUndo &undo) {
  append(json::Object().raw("Metadata", undo.toJson()).compact(), false);
  sync();
}

void MoveJournal::commit(const std::function<void()> &checkpoint) {
  syncAlbums(header_);
  append(json::string(QStringLiteral("Committed")), true);
  sync();
  checkpoint();
}

void MoveJournal::finish(const std::function<void(const QString &)> &checkpoint) const {
  const auto [events, valid] = readEvents(staging_);
  if (events.empty() || events.back().type != Event::Type::Committed) fail(QStringLiteral("Move is not committed"));
  finishArtifacts(header_, events, checkpoint);
  cleanup(staging_, peer_);
}

void MoveJournal::rollback(const metadata::MetadataWriteGuard *guard) {
  const auto [events, valid] = readEvents(staging_);
  validateUndo(header_, events, guard);
  rollbackFiles(header_, events);
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->type != Event::Type::Metadata) continue;
    if (!guard) fail(QStringLiteral("Missing metadata lease"));
    metadata::restoreMoveMetadataWithHooks(header_.source, header_.target, it->undo, *guard, [](const QString &) {});
  }
  if (!events_->resize(valid)) fail(events_->errorString());
  events_->seek(valid);
  failed_ = false;
  syncAlbums(header_);
  append(json::string(QStringLiteral("RolledBack")), true);
  sync();
  cleanup(staging_, peer_);
}

bool hasJournal(const QString &album) {
  for (const DirEntry &entry : readDir(album))
    if (entry.name.startsWith(kPrefix) && entry.isDir) return true;
  return false;
}

void recoverAlbum(const QString &album) { recoverAlbumWithHook(album, [](const QString &) {}); }

void recoverAlbumWithHook(const QString &requested, const std::function<void(const QString &)> &checkpoint) {
  const QString album = canonical(requested);
  auto localTree = album_tree::read({album});
  QStringList stagingDirs;
  for (const DirEntry &entry : readDir(album))
    if (entry.name.startsWith(kPrefix) && entry.isDir) stagingDirs << path::join(album, entry.name);
  for (const QString &locator : stagingDirs) {
    // A live constructor also holds this lease. Do not inspect or remove its temporarily empty locator while it is
    // still creating the header.
    Header header;
    {
      auto local = media_delete::lockMaintenance(album);
      localTree->ensureCurrent({album});
      if (!exists(locator)) continue;
      if (listDir(locator).isEmpty()) {
        removeKnownDirectory(locator, {});
        continue;
      }
      if (!exists(path::join(locator, kHeader))) {
        quarantineIncomplete(locator);
        continue;
      }
      header = readHeader(locator);
    }
    checkpoint(QStringLiteral("move_recovery_header_read"));
    if (!exists(locator)) continue;
    try {
      header.validate(album);
    } catch (const Error &) {
      if (!exists(locator)) continue;
      throw;
    }
    const QString name = path::fileName(locator);
    const QString staging = path::join(header.source, name), peer = path::join(header.target, name);
    QStringList dirs{header.source, header.target};
    std::sort(dirs.begin(), dirs.end());
    std::unique_ptr<album_tree::TreeLease> tree;
    try {
      tree = album_tree::read(dirs);
    } catch (const Error &) {
      if (!exists(locator)) continue;
      throw;
    }
    std::vector<std::unique_ptr<FileLock>> maintenance;
    for (const QString &dir : dirs) maintenance.push_back(media_delete::lockMaintenance(dir));
    // The originating operation can finish while these leases are awaited.
    if (!exists(locator)) continue;
    header.validate(album);
    tree->ensureCurrent(dirs);
    if (!(readHeader(staging) == header)) fail(QStringLiteral("Move journal header mismatch"));
    const auto [events, valid] = readEvents(staging);
    const bool settled = !events.empty() && events.back().settles();
    if (exists(peer)) {
      std::optional<Header> peerHeader;
      try {
        peerHeader = readHeader(peer);
      } catch (const Error &) {
      }
      if (peerHeader && *peerHeader == header) {
        if (!exists(path::join(staging, kEvents))) fail(QStringLiteral("Move events missing"));
      } else if (peerHeader && events.empty()) {
        removeKnownDirectory(staging, {kEvents, kHeader});
        continue;
      } else if (!peerHeader && events.empty()) {
        quarantineIncomplete(peer);
        removeKnownDirectory(staging, {kEvents, kHeader});
        continue;
      } else {
        fail(QStringLiteral("Move peer header mismatch"));
      }
    } else if (!settled && !events.empty()) {
      fail(QStringLiteral("Move locator missing; preserving originals"));
    }
    bool hasMetadata = false;
    for (const Event &event : events) hasMetadata |= event.type == Event::Type::Metadata;
    hasMetadata &= !settled;
    std::unique_lock<std::mutex> moving, store;
    std::optional<metadata::MetadataWriteGuard> guard;
    if (hasMetadata) {
      auto locked = metadata::lockMetadataForMoveWithTree(dirs, *tree);
      moving = std::move(locked.first);
      guard = std::move(locked.second);
    } else {
      moving = std::unique_lock(mediaMoveLock);
      store = std::unique_lock(storeWriteLock);
    }
    if (!settled) {
      validateUndo(header, events, guard ? &*guard : nullptr);
      rollbackFiles(header, events);
      checkpoint(QStringLiteral("move_recovery_files_restored"));
      for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->type != Event::Type::Metadata) continue;
        if (!guard) fail(QStringLiteral("Missing metadata lease"));
        metadata::restoreMoveMetadataWithHooks(header.source, header.target, it->undo, *guard, checkpoint);
      }
      syncAlbums(header);
      const QString eventsPath = path::join(staging, kEvents);
      if (exists(eventsPath)) {
        if (lstat(eventsPath).isSymlink) fail(QStringLiteral("Move journal is not a regular file: %1").arg(eventsPath));
        QFile file(eventsPath);
        if (!file.open(QIODevice::ReadWrite | QIODevice::Unbuffered)) fail(file.errorString());
        if (!file.resize(valid)) fail(file.errorString());
        file.seek(valid);
        if (file.write("\"RolledBack\"\n") != 13) fail(file.errorString());
        syncHandle(file);
        checkpoint(QStringLiteral("move_recovery_settled"));
      }
    }
    if (!events.empty() && events.back().type == Event::Type::Committed) finishArtifacts(header, events, checkpoint);
    removeKnownDirectory(peer, {kHeader});
    checkpoint(QStringLiteral("move_recovery_peer_removed"));
    removeKnownDirectory(staging, {kEvents, kHeader});
  }
}

}  // namespace core::media_move
