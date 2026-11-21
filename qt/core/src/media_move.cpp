#include "media_move.h"
#include "file_access.h"

#include "album_tree.h"
#include "json.h"
#include "media_delete.h"
#include "thumb.h"
#include "util.h"
#include "vault.h"

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

QByteArray eventBytes(const QString &staging, const QByteArray &bytes, bool encrypt) {
  bool inVault = false;
  const auto keys = vault::keysFor(staging, &inVault);
  if (!inVault) return bytes;
  if (!keys) fail(QStringLiteral("Unlock the library to recover its move journal"));
  const QByteArray ad = QByteArrayLiteral("room237 move journal v1|") + path::fileName(staging).toUtf8();
  if (encrypt) return QByteArrayLiteral("enc1:") + vault::seal(keys->rows, bytes, ad).toBase64();
  if (!bytes.startsWith("enc1:")) fail(QStringLiteral("Unauthenticated move journal; preserved for recovery"));
  const auto plain = vault::open(keys->rows, QByteArray::fromBase64(bytes.mid(5)), ad);
  if (!plain) fail(QStringLiteral("Damaged move journal; preserved for recovery"));
  return *plain;
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
    const Event event = parseEvent(eventBytes(staging, bytes.mid(at, end - at), false));
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
    if (entry.isDir && (entry.name.startsWith("claim-") || entry.name.startsWith("rollback-"))) {
      auto parent = FileAccess::current(p)->directory(p);
      auto child = parent->childForRemoval(entry.name);
      child.requirePrivate();
      parent->removeEmpty(entry.name, &child);
      continue;
    }
    bool known = false;
    for (const QString &name : names) known |= entry.name == name;
    if (!known || !entry.isFile) fail(QStringLiteral("Unknown files in move journal; preserving them"));
  }
  for (const QString &name : names) removeFileIfExists(path::join(p, name));
  const QString parent = path::parent(p);
  auto directory = FileAccess::current(p)->directory(parent);
  auto admitted = FileAccess::current(p)->directory(p);
  directory->removeEmpty(path::fileName(p), admitted.get());
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

QString claimDirectory(const QString &staging, const FileRecord &file, bool rollback);
void transferFile(const QString &staging, const FileRecord &file, const QString &from, const QString &sourceName,
                  const QString &to, const QString &targetName, bool rollback, const std::function<void(const QString &)> &checkpoint);
bool retainedClaims(const QString &staging);

void finishArtifacts(const Header &header, const std::vector<Event> &events, const std::function<void(const QString &)> &checkpoint, const QString &staging) {
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
    const FileRecord record{QStringLiteral("artifact:") + kindName(artifact.kind) + QLatin1Char(':') + artifact.sourceFile, artifact.targetFile, artifact.revision};
    const QString claimed = path::join(claimDirectory(staging, record, false), "entry");
    if (regularRevision(source) != artifact.revision && !entryExists(claimed)) continue;
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
    try {
      transferFile(staging, record, sourceDir, artifact.sourceFile, targetDir, artifact.targetFile, false, checkpoint);
      checkpoint(QStringLiteral("move_artifact_renamed"));
    } catch (const Error &) {
      if (entryExists(target) && !retainedClaims(staging)) qWarning("Preserving conflicting move artifact %s", qUtf8Printable(source));
      else throw;
    }
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

QString claimDirectory(const QString &staging, const FileRecord &file, bool rollback) {
  return path::join(staging, QString(rollback ? "rollback-" : "claim-") + sha256Hex(file.source.toUtf8()));
}

// The existing File record describes both the claim and its recovery: a deterministic owner-only subdirectory
// retains an exchanged entry until the journal can be reconciled. No new event type or journal version is needed.
void transferFile(const QString &staging, const FileRecord &file, const QString &from, const QString &sourceName,
                  const QString &to, const QString &targetName, bool rollback, const std::function<void(const QString &)> &checkpoint) {
  auto *view = FileAccess::current(from);
  auto source = view->directory(from), destination = FileAccess::current(to)->directory(to);
  const QString claimPath = claimDirectory(staging, file, rollback);
  auto parent = view->directory(staging);
  auto claim = parent->optionalChild(path::fileName(claimPath), true);
  if (!claim) claim.emplace(parent->privateChild(path::fileName(claimPath)));
  claim->requirePrivate();
  auto shared = std::make_shared<Directory>(claim->share()); view->bindDirectory(claimPath, shared);
  const QString logicalSource = path::join(from, sourceName), logicalTarget = path::join(to, targetName);
  std::shared_ptr<QFile> opened;
  FileInfo expected;
  if (claim->info("entry").exists) {
    if (source->info(sourceName).exists) fail(QStringLiteral("Move claim and source both exist; preserving journal"));
    opened = claim->openRegular("entry", QIODevice::ReadOnly, true);
    expected = openedFileInfo(*opened);
  } else {
    opened = view->pin(logicalSource, true);
    expected = openedFileInfo(*opened);
    if (!expected.isFile || metadata::sourceRevisionFromInfo(expected) != file.revision) fail(QStringLiteral("Move source changed before claim"));
    checkpoint(rollback ? QStringLiteral("before_rollback_claim") : QStringLiteral("before_move_claim"));
    source->claim(sourceName, *claim, "entry", expected, opened.get());
    view->unpin(logicalSource);
  }
  if (!expected.isFile || metadata::sourceRevisionFromInfo(expected) != file.revision) fail(QStringLiteral("Move claim changed; preserving it"));
  checkpoint(rollback ? QStringLiteral("before_rollback_publication") : QStringLiteral("before_move_publication"));
  const auto named = claim->info("entry");
  if (!named.isFile || named.dev != expected.dev || named.ino != expected.ino || metadata::sourceRevisionFromInfo(named) != file.revision)
    fail(QStringLiteral("Move claim changed before publication; preserving it"));
  checkpoint(rollback ? QStringLiteral("before_private_rollback_publication") : QStringLiteral("before_private_move_publication"));
  QString error;
  if (!claim->renameTo("entry", *destination, targetName, false, &error, opened.get())) {
    // Preserve no-clobber behavior on the normal collision path. Restore only the admitted object, and never replace
    // a source recreated by another writer. Failure leaves the claim and original File record for recovery.
    QString restore;
    if (!source->info(sourceName).exists && claim->renameTo("entry", *source, sourceName, false, &restore, opened.get())) {
      const auto restored = source->info(sourceName);
      if (restored.dev != expected.dev || restored.ino != expected.ino || metadata::sourceRevisionFromInfo(restored) != file.revision) {
        if (restored.exists) source->claim(sourceName, *claim, "entry", restored);
        fail(QStringLiteral("Move collision rollback changed; preserving journal"));
      }
      source->sync(); claim->sync(); parent->removeEmpty(path::fileName(claimPath), &*claim);
      view->unbindDirectory(claimPath);
    }
    fail(error);
  }
  const auto published = destination->info(targetName);
  if (!published.isFile || published.dev != expected.dev || published.ino != expected.ino || metadata::sourceRevisionFromInfo(published) != file.revision) {
    if (published.exists) destination->claim(targetName, *claim, "entry", published);
    fail(QStringLiteral("Move publication changed; preserved with journal"));
  }
  FileAccess::current(to)->bindPublished(logicalTarget, opened, destination);
  source->sync(); destination->sync(); claim->sync();
  parent->removeEmpty(path::fileName(claimPath), &*claim);
  view->unbindDirectory(claimPath);
  FileAccess::current(to)->releasePublished(logicalTarget);
}

bool retainedClaims(const QString &staging) {
  for (const auto &entry : readDir(staging))
    if ((entry.name.startsWith("claim-") || entry.name.startsWith("rollback-")) && entry.isDir && !listDir(path::join(staging, entry.name)).empty()) return true;
  return false;
}

void rollbackFiles(const Header &header, const std::vector<Event> &events, const QString &staging,
                   const std::function<void(const QString &)> &checkpoint = [](const QString &) {}) {
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
    const QString claimed = path::join(claimDirectory(staging, file, false), "entry"), restoring = path::join(claimDirectory(staging, file, true), "entry");
    if (lstat(original).exists || (!sameRegular(moved, file.revision) && !sameRegular(claimed, file.revision) && !sameRegular(restoring, file.revision))) errors << QStringLiteral("Move originals changed; preserving %1 and %2").arg(file.source, file.target);
  }
  if (!errors.isEmpty()) fail(errors.join(QStringLiteral("; ")));
  for (const FileRecord &file : files) {
    const QString original = path::join(header.source, file.source), moved = path::join(header.target, file.target);
    if (sameRegular(original, file.revision)) continue;
    try {
      const QString claimed = claimDirectory(staging, file, false);
      if (sameRegular(path::join(claimed, "entry"), file.revision)) {
        auto *view = FileAccess::current(header.source);
        auto claim = view->directory(claimed), destination = view->directory(header.source);
        auto opened = claim->openRegular("entry", QIODevice::ReadOnly, true);
        checkpoint(QStringLiteral("before_rollback_publication"));
        const auto expected = openedFileInfo(*opened), named = claim->info("entry");
        if (expected.dev != named.dev || expected.ino != named.ino || metadata::sourceRevisionFromInfo(named) != file.revision) fail(QStringLiteral("Recovery claim changed"));
        checkpoint(QStringLiteral("before_private_rollback_publication"));
        QString error;
        if (!claim->renameTo("entry", *destination, file.source, false, &error, opened.get())) fail(error);
        const auto restored = destination->info(file.source);
        if (restored.dev != expected.dev || restored.ino != expected.ino || metadata::sourceRevisionFromInfo(restored) != file.revision) {
          if (restored.exists) destination->claim(file.source, *claim, "entry", restored);
          fail(QStringLiteral("Recovery publication changed; preserving journal"));
        }
        view->bindPublished(original, opened, destination);
        destination->sync(); claim->sync();
        // The empty container is removed by cleanup below; unknown entries always keep the journal.
      } else transferFile(staging, file, header.target, file.target, header.source, file.source, true, checkpoint);
    } catch (const Error &error) { errors << QStringLiteral("Cannot restore %1: %2").arg(file.source, error.message()); }
  }
  syncDirectory(header.source);
  syncDirectory(header.target);
  if (!errors.isEmpty()) fail(errors.join(QStringLiteral("; ")));
  if (retainedClaims(staging)) fail(QStringLiteral("Retained move claims require recovery; preserving journal"));
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
  if (!FileAccess::current(source) || !FileAccess::current(target)) journal->filesystem_ = std::make_unique<FileAccess>(QStringList{source, target});
  journal->header_ = {1, source, target, directoryId(source), directoryId(target)};
  const QByteArray bytes = headerJson(journal->header_).toUtf8();
  if (bytes.size() > kHeaderLimit) fail(QStringLiteral("Move header too large"));
  TempDir staging(source, kPrefix);
  journal->staging_ = staging.path();
  journal->stagingCapability_ = FileAccess::current(source)->directory(journal->staging_);
  staging.release();
  journal->peer_ = path::join(target, path::fileName(journal->staging_));
  // The peer is a locator, not another independently committed journal.
  atomicWriteExisting(path::join(journal->staging_, kHeader), bytes);
  const QString events = path::join(journal->staging_, kEvents);
  journal->events_ = std::make_unique<QFile>(events);
  if (!openFile(*journal->events_, events, QIODevice::ReadWrite | QIODevice::NewOnly | QIODevice::Unbuffered)) fail(journal->events_->errorString());
  journal->peerCapability_ = std::make_shared<Directory>(FileAccess::current(target)->directory(target)->privateChild(path::fileName(journal->peer_)));
  FileAccess::current(target)->bindDirectory(journal->peer_, journal->peerCapability_);
  atomicWriteExisting(path::join(journal->peer_, kHeader), bytes);
  for (const QString &dir : {journal->staging_, journal->peer_, source, target}) syncDirectory(dir);
  return journal;
}

void MoveJournal::append(const QString &json, bool terminal) {
  if (failed_) fail(QStringLiteral("Move journal write failed"));
  const QByteArray bytes = eventBytes(staging_, json.toUtf8(), true) + '\n';
  const qint64 current = events_->size();
  const qint64 limit = terminal ? kJournalLimit : kJournalLimit - 512;
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
  finishArtifacts(header_, events, checkpoint, staging_);
  cleanup(staging_, peer_);
}

void MoveJournal::rollback(const metadata::MetadataWriteGuard *guard, const std::function<void(const QString &)> &checkpoint) {
  const auto [events, valid] = readEvents(staging_);
  validateUndo(header_, events, guard);
  rollbackFiles(header_, events, staging_, checkpoint);
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
void MoveJournal::moveFile(const FileRecord &file, const std::function<void(const QString &)> &checkpoint) {
  transferFile(staging_, file, header_.source, file.source, header_.target, file.target, false, checkpoint);
}
bool MoveJournal::hasRetainedClaims() const { return retainedClaims(staging_); }
void MoveJournal::restoreFile(const FileRecord &file) {
  Event event; event.type = Event::Type::File; event.file = file;
  rollbackFiles(header_, {event}, staging_);
}

bool hasJournal(const QString &album) {
  for (const DirEntry &entry : readDir(album))
    if (entry.name.startsWith(kPrefix) && entry.isDir) return true;
  return false;
}

void recoverAlbum(const QString &album) { recoverAlbumWithHook(album, [](const QString &) {}); }

void recoverAlbumWithHook(const QString &requested, const std::function<void(const QString &)> &checkpoint) {
  const QString album = canonical(requested);
  FileAccess localAccess({album});
  auto localTree = album_tree::read({album});
  QStringList stagingDirs;
  for (const DirEntry &entry : readDir(album))
    if (entry.name.startsWith(kPrefix) && entry.isDir) stagingDirs << path::join(album, entry.name);
  for (const QString &locator : stagingDirs) {
    auto locatorCapability = std::make_shared<Directory>(localAccess.directory(album)->childForRemoval(path::fileName(locator)));
    localAccess.bindDirectory(locator, locatorCapability);
    localAccess.observeDirectory(locator);
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
    FileAccess peerAccess({header.source, header.target});
    header.validate(album);
    if (staging != locator) peerAccess.bindDirectory(staging, std::make_shared<Directory>(peerAccess.directory(header.source)->childForRemoval(name)));
    if (peer != locator && exists(peer)) peerAccess.bindDirectory(peer, std::make_shared<Directory>(peerAccess.directory(header.target)->childForRemoval(name)));
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
      rollbackFiles(header, events, staging, checkpoint);
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
        if (!openFile(file, eventsPath, QIODevice::ReadWrite | QIODevice::Unbuffered)) fail(file.errorString());
        if (!file.resize(valid)) fail(file.errorString());
        file.seek(valid);
        if (file.write("\"RolledBack\"\n") != 13) fail(file.errorString());
        syncHandle(file);
        checkpoint(QStringLiteral("move_recovery_settled"));
      }
    }
    if (!events.empty() && events.back().type == Event::Type::Committed) finishArtifacts(header, events, checkpoint, staging);
    removeKnownDirectory(peer, {kHeader});
    checkpoint(QStringLiteral("move_recovery_peer_removed"));
    removeKnownDirectory(staging, {kEvents, kHeader});
  }
}

}  // namespace core::media_move
