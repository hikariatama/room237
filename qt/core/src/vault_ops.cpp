#include "vault_ops.h"
#include "file_access.h"
#include "directory_move.h"

#include "album.h"
#include "album_db.h"
#include "album_tree.h"
#include "catalog.h"
#include "constants.h"
#include "duplicates.h"
#include "factors.h"
#include "host.h"
#include "media_delete.h"
#include "media_move.h"
#include "metadata.h"
#include "preload.h"
#include "thumb.h"
#include "util.h"
#include "vault.h"
#include "xattr.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QCryptographicHash>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace core::vault_ops {

namespace {
constexpr quint32 kMemoryKiB = 262144, kPasses = 3;
const QString kJournal = QStringLiteral(".room237-vault-journal");
constexpr const char *kTimestampAttribute = "user.room237.shoot_ts";

// The password-derived key stays in memory until the app quits, so unlocking after a lock needs only the factor.
std::mutex passwordsMutex;
std::map<QString, vault::Secret> passwords;
std::mutex passMutex;
QSet<QString> running;

vault::VaultFile loadVault(const QString &root) {
  auto file = vault::VaultFile::load(root);
  if (!file) fail(QStringLiteral("This root is not encrypted"));
  return *file;
}

vault::Keys keysFrom(const vault::Secret &master, const QString &id) {
  vault::Keys keys;
  keys.master = master;
  keys.rows = vault::derive(master, QByteArrayLiteral("room237 rows v1"));
  keys.names = vault::derive(master, QByteArrayLiteral("room237 names v1"));
  keys.id = id;
  return keys;
}

void emitProgress(const QString &root, const QString &phase, int done, int total, const QString &error = {}) {
  QJsonObject payload{{QStringLiteral("root"), root}, {QStringLiteral("phase"), phase}, {QStringLiteral("done"), done}, {QStringLiteral("total"), total}};
  if (!error.isEmpty()) payload.insert(QStringLiteral("error"), error);
  host::emitEvent(QStringLiteral("vault-progress"), payload);
}

std::optional<vault::Secret> tryOpen(const vault::VaultFile &file, const vault::Secret &p, const QString &kind, const vault::Secret &s) {
  for (const vault::Slot &slot : file.keySlots) {
    if (slot.kind != kind) continue;
    if (auto master = vault::open(vault::slotKey(p, kind, s), slot.wrap, file.id.toUtf8())) return vault::Secret(*master);
  }
  return std::nullopt;
}

const vault::Slot *slotFor(const vault::VaultFile &file, const QString &kind) {
  for (const vault::Slot &slot : file.keySlots)
    if (slot.kind == kind) return &slot;
  return nullptr;
}

// Media of one album that still need encrypting or decrypting.
QStringList albumsOf(const QString &root) {
  QStringList albums = album::walkAlbumPaths(root);
  albums.prepend(root);
  if (auto *view = FileAccess::current(root)) for (const QString &album : albums) view->observeDirectory(album);
  return albums;
}

QJsonObject attributesOf(const QString &file) {
  QJsonObject out;
  for (const QString &name : xattr::list(file)) {
    if (name == QLatin1String(kTimestampAttribute)) continue;
    const xattr::Result value = xattr::get(file, name.toUtf8().constData());
    if (!value.value) fail(QStringLiteral("Cannot read file attribute; original retained"));
    if (value.value) out.insert(name, QString::fromLatin1(value.value->toBase64()));
  }
  return out;
}

// --- Journal: one line per file conversion; a crash finishes or undoes the started ones. ----------------------------

// Workers of a pass append to the same journals. Never destroyed: detached workers may hold it.
std::mutex &journalMutex = *new std::mutex;

// Records are sealed: they hold original names.
void appendJournal(const vault::Keys &keys, const QString &album, const QJsonObject &record) {
  std::lock_guard lock(journalMutex);
  QFile file(path::join(album, kJournal));
  if (!openFile(file, path::join(album, kJournal), QIODevice::WriteOnly | QIODevice::Append)) fail(file.errorString());
  const QByteArray bytes = vault::seal(keys.rows, QJsonDocument(record).toJson(QJsonDocument::Compact), QByteArrayLiteral("journal")).toBase64() + '\n';
  if (file.write(bytes) != bytes.size()) fail(file.errorString());
  syncOpenedFile(file);
}
std::vector<QJsonObject> readJournal(const vault::Keys &keys, const QString &album) {
  std::vector<QJsonObject> out;
  QFile file(path::join(album, kJournal));
  if (!entryExists(path::join(album, kJournal))) return out;
  if (!openFile(file, path::join(album, kJournal), QIODevice::ReadOnly)) fail(file.errorString());
  for (const QByteArray &line : file.readAll().split('\n')) {
    if (line.trimmed().isEmpty()) continue;
    const auto plain = vault::open(keys.rows, QByteArray::fromBase64(line.trimmed()), QByteArrayLiteral("journal"));
    if (!plain) fail(QStringLiteral("Damaged conversion journal; preserved for recovery"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(*plain, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail(QStringLiteral("Invalid conversion journal; preserved for recovery"));
    const auto record = document.object();
    if (record.contains("done")) {
      if (record.size() != 1 || !validFileName(record.value("done").toString())) fail(QStringLiteral("Invalid conversion completion"));
    } else if ((record.size() != 3 && record.size() != 5) || !validFileName(record.value("plain").toString()) || !constants::isEncryptedName(record.value("id").toString()) ||
               (record.value("op").toString() != QLatin1String("encrypt") && record.value("op").toString() != QLatin1String("decrypt"))) fail(QStringLiteral("Invalid conversion record"));
    out.push_back(record);
  }
  if (file.error() != QFileDevice::NoError) fail(file.errorString());
  return out;
}

QString uniqueName(const QString &dir, const QString &name, const QSet<QString> &reserved) {
  if (!entryExists(path::join(dir, name)) && !reserved.contains(name)) return name;
  const int dot = name.lastIndexOf(QLatin1Char('.'));
  const QString stem = dot > 0 ? name.left(dot) : name, ext = dot > 0 ? name.mid(dot) : QString();
  for (int counter = 1;; ++counter) {
    const QString candidate = QStringLiteral("%1_%2%3").arg(stem).arg(counter).arg(ext);
    if (!entryExists(path::join(dir, candidate)) && !reserved.contains(candidate)) return candidate;
  }
}

QString databaseOf(const QString &album) { return metadata::albumMetaPath(album); }

// Brings a moved row's freshness markers to the file now at `file`, so its cached previews and metadata stay current.
void refreshRevisions(FileMetaEntry &entry, const QString &file, const QString &thumbDir) {
  const auto source = metadata::sourceRevision(file);
  if (entry.metaSourceRevision) entry.metaSourceRevision = source;
  if (entry.metaFailedSourceRevision) entry.metaFailedSourceRevision = source;
  if (entry.thumbFailedSourceRevision) entry.thumbFailedSourceRevision = source;
  entry.metaEmbeddedRevision.reset();
  if (entry.thumbVersion) {
    entry.thumbSourceRevision = source;
    entry.thumbHashRevision = thumb::contentRevision(file);
    entry.thumbSourceHash = thumb::sourceHash(file);
  }
  if (!isFile(thumb::thumbPath(file, thumbDir))) {
    entry.thumbVersion.reset();
    entry.blurPreview.reset();
  }
  if (entry.hash || entry.hashFailed) {
    entry.hashSourceRevision = metadata::mediaRevision(file);
    if (entry.hashFailedSourceRevision) entry.hashFailedSourceRevision = source;
  }
}

// --- Encryption ----------------------------------------------------------------------------------------------------

bool sameObject(const FileInfo &a, const FileInfo &b) {
  return a.exists && b.exists && a.dev == b.dev && a.ino == b.ino && a.isFile == b.isFile &&
      a.isSymlink == b.isSymlink && a.size == b.size && a.mtimeNs == b.mtimeNs;
}

// The sealed conversion record locates a retained original, verified against the complete ciphertext before deletion.
class ConversionSource {
public:
  ConversionSource(const vault::Keys &keys, const QString &album, const QString &plainName, const QString &id, bool encrypt)
      : view_(FileAccess::current(album)), album_(view_->directory(album)),
        original_(encrypt ? plainName : id), claimName_(QStringLiteral(".room237-vault-claim-") + id),
        entry_(encrypt ? QStringLiteral("plain") : QStringLiteral("sealed")), logical_(path::join(album, original_)) {
    const QString claimPath = path::join(album, claimName_);
    if (auto bound = view_->boundDirectory(claimPath)) staging_.emplace(bound->share());
    else staging_ = album_->optionalChild(claimName_, true);
    if (staging_) { staging_->requirePrivate(); view_->bindDirectory(claimPath, std::make_shared<Directory>(staging_->share())); }
    if (staging_ && staging_->info(entry_).exists) {
      if (album_->info(original_).exists) fail(QStringLiteral("Conversion original and retained claim both exist; preserving them"));
      file_ = view_->pinned(logical_);
      if (!file_ || !file_->isOpen()) file_ = staging_->openRegular(entry_, QIODevice::ReadOnly, true);
      claimed_ = true;
    } else if (album_->info(original_).exists) {
      file_ = view_->open(logical_, QIODevice::ReadOnly);
    }
    if (file_) view_->bind(logical_, file_);
  }
  bool exists() const { return bool(file_); }
  bool claimed() const { return claimed_; }
  void remove(const std::function<void()> &verifyDestination) {
    if (!file_) {
      if (staging_) {
        if (!staging_->entries().empty()) fail(QStringLiteral("Unknown conversion claim entries; preserving journal"));
        album_->removeEmpty(claimName_, &*staging_);
      }
      return;
    }
    const auto expected = openedFileInfo(*file_);
    if (!staging_) staging_.emplace(album_->privateChild(claimName_));
    if (!claimed_) {
      view_->checkpoint(QStringLiteral("before_source_claim"), logical_);
#ifdef Q_OS_WIN
      view_->releaseReaders(expected);
      // FFmpeg's pathname reader is protected by a no-delete-sharing READ handle. Once all decoders have returned,
      // claim with a fresh no-follow DELETE handle and verify the original id; never reopen for decoding afterward.
      album_->claim(original_, *staging_, entry_, expected);
      file_ = staging_->openRegular(entry_, QIODevice::ReadOnly, true);
      view_->bind(logical_, file_);
#else
      album_->claim(original_, *staging_, entry_, expected, file_.get());
#endif
      claimed_ = true;
    }
    view_->checkpoint(QStringLiteral("before_claimed_unlink"), logical_);
    verifyDestination();
    if (!sameObject(staging_->info(entry_), openedFileInfo(*file_))) fail(QStringLiteral("Conversion claim changed; preserving it"));
    staging_->removeFileIfPresent(entry_, &expected, file_.get());
    staging_->sync();
    view_->unpin(logical_);
    // Unknown or exchanged entries are retained with the original conversion journal.
    if (!staging_->entries().empty()) fail(QStringLiteral("Unknown entries in conversion claim; preserving journal"));
    album_->removeEmpty(claimName_, &*staging_);
    album_->sync();
  }
private:
  FileAccess *view_;
  std::shared_ptr<Directory> album_;
  std::optional<Directory> staging_;
  QString original_, claimName_, entry_, logical_;
  std::shared_ptr<QFile> file_;
  bool claimed_ = false;
};

void validateConversionContent(const vault::Keys &keys, const QString &plain, const QString &sealed) {
  QFile source;
  if (!openFile(source, plain, QIODevice::ReadOnly)) fail(source.errorString());
  vault::DecryptingDevice encrypted(std::make_shared<vault::Keys>(keys), sealed);
  if (!encrypted.open(QIODevice::ReadOnly)) fail(encrypted.errorString());
  if (source.size() != encrypted.size()) fail(QStringLiteral("Conversion original changed; preserving both copies"));
  while (!source.atEnd()) {
    QByteArray bytes = source.read(1 << 20), decoded = encrypted.read(bytes.size());
    const bool equal = !bytes.isEmpty() && decoded == bytes;
    bytes.fill('\0'); decoded.fill('\0');
    if (!equal) fail(QStringLiteral("Conversion content changed; preserving both copies"));
  }
  if (source.error() != QFileDevice::NoError || encrypted.pos() != encrypted.size()) fail(QStringLiteral("Cannot verify conversion content"));
}

void recoverPublication(const vault::Keys &keys, const QString &album, const QString &plainName, const QString &id, bool encrypt) {
  auto *view = FileAccess::current(album);
  auto parent = view->directory(album);
  const QString target = encrypt ? id : plainName;
  const QString prefix = QStringLiteral(".room237-vault-tmp-") + (encrypt ? QStringLiteral("encrypt-") : QStringLiteral("decrypt-")) + sha256Hex(id.toUtf8()) + QLatin1Char('-');
  for (const auto &entry : parent->entries()) {
    if (!entry.name.startsWith(prefix)) continue;
    if (!entry.isDir) fail(QStringLiteral("Conversion staging changed; preserving journal"));
    auto staging = parent->childForRemoval(entry.name);
    staging.requirePrivate();
    if (!staging.info("published").exists) continue;  // Publication had not started; the original remains authoritative.
    const QString staged = path::join(path::join(album, entry.name), "published");
    auto opened = staging.openRegular("published", QIODevice::ReadOnly, true);
    view->bind(staged, opened);
    if (encrypt) {
      if (vault::readMeta(keys, staged).name != plainName) fail(QStringLiteral("Retained ciphertext belongs to another original; preserving journal"));
      const QString original = path::join(album, plainName);
      if (view->info(original).exists) validateConversionContent(keys, original, staged);
      else fail(QStringLiteral("Original missing during publication recovery; preserving journal"));
    } else validateConversionContent(keys, staged, path::join(album, id));
    const auto expected = openedFileInfo(*opened), named = staging.info("published");
    if (!sameObject(expected, named)) fail(QStringLiteral("Retained publication changed; preserving journal"));
    view->checkpoint(QStringLiteral("before_recovery_publication"), staged, path::join(album, target));
    QString error;
    if (!staging.renameTo("published", *parent, target, false, &error, opened.get())) fail(error);
    const auto published = parent->info(target);
    if (!sameObject(expected, published)) {
      if (published.exists) parent->claim(target, staging, "published", published);
      fail(QStringLiteral("Recovered publication changed; preserving journal"));
    }
    parent->sync(); staging.sync(); view->bindPublished(path::join(album, target), opened, parent);
    if (!staging.entries().empty()) fail(QStringLiteral("Unknown conversion staging entries; preserving journal"));
    parent->removeEmpty(entry.name, &staging);
  }
}

void finishEncrypted(const vault::Keys &keys, const QString &album, const QString &plainName, const QString &id) {
  const QString plain = path::join(album, plainName), target = path::join(album, id);
  ConversionSource source(keys, album, plainName, id, true);
  auto *view = FileAccess::current(album);
  auto sealed = view->open(target, QIODevice::ReadOnly);
  view->bind(target, sealed);
  if (vault::readMeta(keys, target).name != plainName) fail(QStringLiteral("Conversion target belongs to another original"));
  if (source.exists()) {
    validateConversionContent(keys, plain, target);
    if (attributesOf(plain) != vault::readMeta(keys, target).xattrs) fail(QStringLiteral("Original attributes changed; preserving original and journal"));
  }
  const QString thumbDir = path::join(album, thumb::kThumbDir);
  const QString plainThumb = thumb::thumbPath(plain, thumbDir);
  const QString sealedThumb = thumb::thumbPath(target, thumbDir);
  if (isFile(plainThumb) && !vault::isEncryptedFile(plainThumb)) {
    vault::encryptBytes(keys, readAll(plainThumb), sealedThumb, vault::FileMeta{path::fileName(sealedThumb)}, true);
  }
  for (const QString &stale : thumb::deletedThumbnailPaths(plainName, thumbDir)) removeFileIfExists(stale);
  if (source.exists()) if (const auto timestamp = metadata::readEmbeddedTimestamp(plain)) metadata::writeEmbeddedTimestamp(target, *timestamp);
  metadata::ensureDatabase(album);
  // After a crash the row may already carry the new name.
  FileMetaEntry entry = album_db::readFile(databaseOf(album), plainName).value_or(album_db::readFile(databaseOf(album), id).value_or(FileMetaEntry{}));
  entry.vaultName = plainName;
  refreshRevisions(entry, target, thumbDir);
  if (const auto timestamp = metadata::readEmbeddedTimestamp(target)) entry.metaEmbeddedTimestamp = timestamp;
  album_db::renameRow(databaseOf(album), plainName, id, entry);
  view->checkpoint(QStringLiteral("after_encrypt_metadata"), plain);
  try {
    catalog::forget(album, {plainName});
    catalog::publish(album, id, entry);
  } catch (const Error &) {
  }
  source.remove([&] {
    if (!sameObject(view->directory(album)->info(id), openedFileInfo(*sealed))) fail(QStringLiteral("Ciphertext destination changed; preserving original and journal"));
  });
  view->checkpoint(QStringLiteral("after_encrypt_source_removal"), plain);
}

void finishDecrypted(const vault::Keys &keys, const QString &album, const QString &id, const QString &name);

void recoverJournal(const vault::Keys &keys, const QString &album) {
  const std::vector<QJsonObject> records = readJournal(keys, album);
  if (records.empty()) return;
  QSet<QString> done;
  for (const QJsonObject &record : records)
    if (record.contains(QLatin1String("done"))) done.insert(record.value(QLatin1String("done")).toString());
  for (const QJsonObject &record : records) {
    if (record.contains(QLatin1String("done"))) continue;
    const QString kind = record.value(QLatin1String("op")).toString();
    const QString plainName = record.value(QLatin1String("plain")).toString(), id = record.value(QLatin1String("id")).toString();
    if (!validFileName(plainName) || !validFileName(id) || done.contains(id)) continue;
    if (kind != QLatin1String("encrypt") && kind != QLatin1String("decrypt")) continue;
    const QString plain = path::join(album, plainName), sealed = path::join(album, id);
    FileAccess recordAccess(FileAccess::capture());
    auto recoveryTree = album_tree::read({album});
    ConversionSource retained(keys, album, plainName, id, kind == QLatin1String("encrypt"));
    if (kind == QLatin1String("encrypt") || kind == QLatin1String("decrypt")) recoverPublication(keys, album, plainName, id, kind == QLatin1String("encrypt"));
    if (kind == QLatin1String("encrypt")) {
      // The sealed copy is published only complete; if it exists, the original's removal is all that may be left.
      bool complete = false;
      try {
        complete = recordAccess.directory(album)->info(id).isFile && vault::readMeta(keys, sealed).name == plainName;
      } catch (const Error &) {
      }
      if (complete) {
        auto maintenance = media_delete::lockMaintenance(album);
        auto [moving, guard] = metadata::lockMetadataForMoveWithTree({album}, *recoveryTree);
        finishEncrypted(keys, album, plainName, id);
      } else if (!retained.exists() || recordAccess.directory(album)->info(id).exists || retained.claimed()) fail(QStringLiteral("Ambiguous encryption recovery; preserving all entries and journal"));
    } else if (kind == QLatin1String("decrypt")) {
      bool restored = false;
      try {
        const auto named = recordAccess.directory(album)->info(plainName);
        restored = named.isFile && retained.exists() && vault::readMeta(keys, sealed).size == named.size;
      } catch (const Error &) {
      }
      if (!retained.exists() && recordAccess.directory(album)->info(plainName).isFile && record.contains("sha256")) {
        QFile restoredFile;
        if (!openFile(restoredFile, plain, QIODevice::ReadOnly)) fail(restoredFile.errorString());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&restoredFile) || QString::fromLatin1(hash.result().toHex()) != record.value("sha256").toString() ||
            QString::number(restoredFile.size()) != record.value("size").toString()) fail(QStringLiteral("Restored content changed; preserving journal"));
        continue;
      }
      if (restored) finishDecrypted(keys, album, id, plainName);
      else if (!retained.exists() || recordAccess.directory(album)->info(plainName).exists || retained.claimed()) fail(QStringLiteral("Ambiguous decryption recovery; preserving all entries and journal"));
    }
  }
  removeFileIfExists(path::join(album, kJournal));
}

bool encryptOne(const vault::Keys &keys, const QString &album, const QString &name, const FileInfo &admitted) {
  const QString plain = path::join(album, name);
  auto *view = FileAccess::current(album);
  view->checkpoint(QStringLiteral("before_source_open"), plain);
  auto input = view->pin(plain);
  const FileInfo before = openedFileInfo(*input);
  if (!sameObject(before, admitted)) fail(QStringLiteral("Original changed after discovery; preserving it"));
  if (!before.isFile) return false;
  // Derived data comes from the plaintext while it is still readable by FFmpeg and ImageIO.
  const AdvancedSettings settings = readSettings();
  try {
    thumb::ensureThumbWithSettings(plain, path::join(album, thumb::kThumbDir), settings);
  } catch (const std::exception &) {
  }
  try {
    metadata::getFileMetadataCachedWithSettings(plain, settings);
  } catch (const std::exception &) {
  }
  if (constants::isImage(plain)) {
    try {
      duplicates::computeHashForFile(plain, settings.hashConfig(), settings);
    } catch (const std::exception &) {
    }
  }
  // The sealed copy is written beside the original under its random name and appears whole; the original stays until
  // the copy is complete. Only the switch from one to the other holds the album's locks, so files encrypt in parallel.
  const QString id = vault::newPhysicalName();
  appendJournal(keys, album, {{QStringLiteral("op"), QStringLiteral("encrypt")}, {QStringLiteral("plain"), name}, {QStringLiteral("id"), id}});
  vault::FileMeta meta;
  meta.name = name;
  meta.mtimeNs = before.mtimeNs;
  meta.birthSec = before.birthSec;
  meta.xattrs = attributesOf(plain);
  view->checkpoint(QStringLiteral("before_encrypt_write"), plain);
  vault::encryptFile(keys, plain, path::join(album, id), meta);
  view->checkpoint(QStringLiteral("after_encrypt_write"), plain);
  auto tree = album_tree::read({album});
  auto maintenance = media_delete::lockMaintenance(album);
  auto [moving, guard] = metadata::lockMetadataForMoveWithTree({album}, *tree);
  // Changed, moved or deleted while it was being encrypted: the copy is dropped and the next pass takes it again.
  const FileInfo now = view->directory(album)->info(name);
  if (!now.isFile || now.mtimeNs != before.mtimeNs || now.size != before.size || now.ino != before.ino || now.dev != before.dev) {
    removeFileIfExists(path::join(album, id));
    appendJournal(keys, album, {{QStringLiteral("done"), id}});
    return false;
  }
  finishEncrypted(keys, album, name, id);
  appendJournal(keys, album, {{QStringLiteral("done"), id}});
  return true;
}

// Records that name the plaintext outside the sealed rows: indexes rebuilt from the sealed data when needed.
void removePlainIndexes(const QString &root, const QStringList &albums) {
  removeFileIfExists(path::join(path::join(root, catalog::kDirectory), QStringLiteral("index.sqlite")));
  removeFileIfExists(path::join(path::join(root, catalog::kDirectory), QStringLiteral("index.sqlite-journal")));
  for (const QString &album : albums) {
    const QString meta = metadata::metaDir(album);
    if (!isDir(meta)) continue;
    for (const DirEntry &entry : readDir(meta))
      if (entry.name.startsWith(QLatin1String("duplicates.sqlite")) || entry.name == QLatin1String("album.json") || entry.name.endsWith(QLatin1String(".meta"))) removeFileIfExists(path::join(meta, entry.name));
  }
}

// Runs `work` for items 0..count-1 on up to preload.encryptWorkers threads. The count is read again whenever a thread
// finishes, so a change applies during the pass. Stops starting new items once `keepGoing` turns false.
void forEachParallel(size_t count, const std::function<void(size_t)> &work, const std::function<bool()> &keepGoing) {
  struct State {
    std::mutex mutex;
    std::condition_variable finished;
    size_t running = 0;
  };
  auto state = std::make_shared<State>();
  size_t next = 0;
  std::unique_lock lock(state->mutex);
  for (;;) {
    const size_t limit = std::clamp<size_t>(readSettings().preload.encryptWorkers, 1, 32);
    while (next < count && state->running < limit && keepGoing()) {
      ++state->running;
      // `work` outlives the thread: this function returns only after every thread has finished it.
      std::thread([state, &work, item = next++] {
        setLowPriorityCurrentThread();
        work(item);
        std::lock_guard done(state->mutex);
        --state->running;
        state->finished.notify_all();
      }).detach();
    }
    if (state->running == 0 && (next >= count || !keepGoing())) return;
    state->finished.wait_for(lock, std::chrono::milliseconds(250));
  }
}

void runEncryption(const QString &root) {
  const auto keys = vault::keysFor(root);
  if (!keys) fail(QStringLiteral("This library is locked"));
  FileAccess access(FileAccess::snapshot(keys->filesystem));
  auto passLease = FileLock::acquireIn(*access.directory(root), QStringLiteral(".room237-vault-setup.lock"), std::chrono::milliseconds(0), std::chrono::milliseconds(5));
  directory_move::recover(root);
  vault::VaultFile file = loadVault(root);
  if (file.id != keys->id) fail(QStringLiteral("Vault identity changed; preserving files"));
  const QStringList albums = albumsOf(root);
  for (const QString &album : albums) recoverJournal(*keys, album);
  std::vector<std::tuple<QString, QString, FileInfo, QString>> work;
  for (const QString &album : albums) {
    auto admitted = access.directory(album);
    for (const DirEntry &entry : admitted->entries())
      if (entry.isFile && constants::isMedia(entry.name) && !constants::isEncryptedName(entry.name))
        work.emplace_back(album, entry.name, admitted->info(entry.name), admitted->identity());
  }
  // After an unlock there is usually nothing left to do.
  if (work.empty() && file.state == QLatin1String("encrypted")) {
    emitProgress(root, QStringLiteral("encrypted"), 0, 0);
    return;
  }
  if (file.state != QLatin1String("encrypting")) {
    file.state = QStringLiteral("encrypting");
    file.save(root);
  }
  std::atomic<int> done = 0;
  emitProgress(root, QStringLiteral("encrypting"), 0, int(work.size()));
  std::mutex errorMutex;
  QString lastError;
  const auto filesystem = FileAccess::capture();
  forEachParallel(work.size(), [&](size_t i) {
    FileAccess worker(filesystem);
    const auto &[album, name, admitted, albumId] = work[i];
    try {
      auto rootTree = album_tree::read({root});
      if (const auto current = worker.locateDirectory(album, albumId)) {
        auto tree = album_tree::read({*current});
        encryptOne(*keys, *current, name, admitted);
      }
    } catch (const std::exception &error) {
      std::lock_guard lock(errorMutex);
      lastError = QString::fromUtf8(error.what());
      qWarning("Cannot encrypt %s: %s", qUtf8Printable(path::join(album, name)), error.what());
    }
    emitProgress(root, QStringLiteral("encrypting"), ++done, int(work.size()));
  }, [&] { return vault::keysFor(root) != nullptr; });
  if (!vault::keysFor(root)) fail(QStringLiteral("This library is locked"));
  FileAccess finishing(FileAccess::snapshot(filesystem));
  for (const QString &album : albumsOf(root)) recoverJournal(*keys, album);
  auto finishingTree = album_tree::write(root);
  const auto currentAlbums = albumsOf(root);
  for (const QString &album : currentAlbums)
    for (const auto &entry : readDir(album))
      if (entry.isFile && constants::isMedia(entry.name) && !constants::isEncryptedName(entry.name))
        lastError = QStringLiteral("Unencrypted media remain; originals and metadata retained. Retry encryption.");
  if (!lastError.isEmpty()) {
    emitProgress(root, QStringLiteral("incomplete"), done.load(), int(work.size()), lastError);
    return;
  }
  for (const QString &album : currentAlbums) {
    auto [moving, guard] = metadata::lockMetadataForMoveWithTree({album}, *finishingTree);
    metadata::ensureDatabase(album);
    album_db::reseal(databaseOf(album), true);
  }
  removePlainIndexes(root, currentAlbums);
  file = loadVault(root);
  file.state = lastError.isEmpty() ? QStringLiteral("encrypted") : QStringLiteral("encrypting");
  file.save(root);
  emitProgress(root, lastError.isEmpty() ? QStringLiteral("encrypted") : QStringLiteral("incomplete"), done.load(), int(work.size()), lastError);
}

// --- Decryption ----------------------------------------------------------------------------------------------------

// Names chosen for restored files that are not on disk yet, per album; workers of one pass share them.
std::mutex &reservedMutex = *new std::mutex;

// The restored file is in place: its attributes, preview, row and catalog entry follow, then the sealed copy goes.
void finishDecrypted(const vault::Keys &keys, const QString &album, const QString &id, const QString &name) {
  const QString sealed = path::join(album, id), plain = path::join(album, name);
  auto tree = album_tree::read({album});
  auto maintenance = media_delete::lockMaintenance(album);
  auto [moving, guard] = metadata::lockMetadataForMoveWithTree({album}, *tree);
  ConversionSource source(keys, album, name, id, false);
  auto *view = FileAccess::current(album);
  auto restored = view->open(plain, QIODevice::ReadOnly);
  view->bind(plain, restored);
  if (source.exists()) validateConversionContent(keys, plain, sealed);
  const vault::FileMeta meta = vault::readMeta(keys, sealed);
  for (auto it = meta.xattrs.constBegin(); it != meta.xattrs.constEnd(); ++it) {
    const QByteArray value = QByteArray::fromBase64(it.value().toString().toLatin1());
    xattr::set(plain, it.key().toUtf8().constData(), value);
    if (xattr::get(plain, it.key().toUtf8().constData()).value != std::optional<QByteArray>(value)) fail(QStringLiteral("Cannot verify restored attribute; ciphertext retained"));
  }
  const auto timestamp = metadata::readEmbeddedTimestamp(sealed);
  const QString thumbDir = path::join(album, thumb::kThumbDir);
  const QString sealedThumb = thumb::thumbPath(sealed, thumbDir), plainThumb = thumb::thumbPath(plain, thumbDir);
  if (isFile(sealedThumb) && vault::isEncryptedFile(sealedThumb)) {
    TempFile temporary(thumbDir, thumb::kTempPrefix, QStringLiteral(".webp"));
    temporary.write(vault::decryptAll(keys, sealedThumb, 64 << 20));
    temporary.persist(plainThumb);
  }
  if (timestamp) metadata::writeEmbeddedTimestamp(plain, *timestamp);
  metadata::ensureDatabase(album);
  FileMetaEntry entry = album_db::readFile(databaseOf(album), id).value_or(album_db::readFile(databaseOf(album), name).value_or(FileMetaEntry{}));
  entry.vaultName.reset();
  refreshRevisions(entry, plain, thumbDir);
  album_db::renameRow(databaseOf(album), id, name, entry);
  try {
    catalog::forget(album, {id});
    catalog::publish(album, name, entry);
  } catch (const Error &) {
  }
  source.remove([&] {
    if (!sameObject(view->directory(album)->info(name), openedFileInfo(*restored))) fail(QStringLiteral("Restored destination changed; preserving ciphertext and journal"));
  });
  view->checkpoint(QStringLiteral("after_decrypt_source_removal"), plain);
  for (const QString &stale : thumb::deletedThumbnailPaths(id, thumbDir)) removeFileIfExists(stale);
}

bool decryptOne(const vault::Keys &keys, const QString &album, const QString &id, const FileInfo &admitted, QSet<QString> &reserved) {
  const QString sealed = path::join(album, id);
  auto *view = FileAccess::current(album);
  view->checkpoint(QStringLiteral("before_source_open"), sealed);
  auto input = view->pin(sealed);
  if (!sameObject(openedFileInfo(*input), admitted)) fail(QStringLiteral("Encrypted original changed after discovery"));
  const vault::FileMeta meta = vault::readMeta(keys, sealed);
  if (!validFileName(meta.name)) fail(QStringLiteral("Invalid original name in %1").arg(sealed));
  QString name;
  {
    std::lock_guard lock(reservedMutex);
    name = uniqueName(album, meta.name, reserved);
    reserved.insert(name);
  }
  appendJournal(keys, album, {{QStringLiteral("op"), QStringLiteral("decrypt")}, {QStringLiteral("plain"), name}, {QStringLiteral("id"), id}, {QStringLiteral("sha256"), QString::fromLatin1(meta.sha256.toHex())}, {QStringLiteral("size"), QString::number(meta.size)}});
  // Written to a temporary file and moved into place whole, outside the album's locks.
  vault::decryptToFile(keys, sealed, path::join(album, name));
  finishDecrypted(keys, album, id, name);
  appendJournal(keys, album, {{QStringLiteral("done"), id}});
  return true;
}

void runDecryption(const QString &root) {
  const auto keys = vault::keysFor(root);
  if (!keys) fail(QStringLiteral("This library is locked"));
  FileAccess access(FileAccess::snapshot(keys->filesystem));
  auto passLease = FileLock::acquireIn(*access.directory(root), QStringLiteral(".room237-vault-setup.lock"), std::chrono::milliseconds(0), std::chrono::milliseconds(5));
  directory_move::recover(root);
  vault::VaultFile file = loadVault(root);
  if (file.id != keys->id) fail(QStringLiteral("Vault identity changed; preserving files"));
  file.state = QStringLiteral("decrypting");
  file.save(root);
  const QStringList albums = albumsOf(root);
  for (const QString &album : albums) recoverJournal(*keys, album);
  std::vector<std::tuple<QString, QString, FileInfo, QString>> work;
  for (const QString &album : albums) {
    auto admitted = access.directory(album);
    for (const DirEntry &entry : admitted->entries())
      if (entry.isFile && constants::isEncryptedName(entry.name)) work.emplace_back(album, entry.name, admitted->info(entry.name), admitted->identity());
  }
  std::atomic<int> done = 0;
  emitProgress(root, QStringLiteral("decrypting"), 0, int(work.size()));
  std::mutex errorMutex;
  QString lastError;
  std::map<QString, QSet<QString>> reserved;
  for (const QString &album : albums) reserved[album];
  const auto filesystem = FileAccess::capture();
  forEachParallel(work.size(), [&](size_t i) {
    FileAccess worker(filesystem);
    const auto &[album, id, admitted, albumId] = work[i];
    try {
      auto rootTree = album_tree::read({root});
      if (const auto current = worker.locateDirectory(album, albumId)) {
        auto tree = album_tree::read({*current});
        decryptOne(*keys, *current, id, admitted, reserved[album]);
      }
    } catch (const std::exception &error) {
      std::lock_guard lock(errorMutex);
      lastError = QString::fromUtf8(error.what());
      qWarning("Cannot decrypt %s: %s", qUtf8Printable(path::join(album, id)), error.what());
    }
    emitProgress(root, QStringLiteral("decrypting"), ++done, int(work.size()));
  }, [&] { return vault::keysFor(root) != nullptr; });
  if (!vault::keysFor(root)) fail(QStringLiteral("This library is locked"));
  FileAccess finishing(FileAccess::snapshot(filesystem));
  const auto currentAlbums = albumsOf(root);
  for (const QString &album : currentAlbums) recoverJournal(*keys, album);
  auto finishingTree = album_tree::write(root);
  FileAccess stable(FileAccess::snapshot(filesystem));
  const auto stableAlbums = albumsOf(root);
  for (const auto &album : stableAlbums)
    for (const auto &entry : readDir(album)) if (!entry.isDir && constants::isEncryptedName(entry.name))
      lastError = QStringLiteral("Media changed during decryption; encrypted originals and keys are retained. Retry removal of encryption.");
  if (!lastError.isEmpty()) {
    // The vault stays, with its keys, until every file is back; the next attempt continues from here.
    emitProgress(root, QStringLiteral("incomplete"), done.load(), int(work.size()), lastError);
    return;
  }
  for (const QString &album : stableAlbums)
    if (exists(databaseOf(album))) {
      auto [moving, guard] = metadata::lockMetadataForMoveWithTree({album}, *finishingTree);
      album_db::reseal(databaseOf(album), false);
    }
  if (loadVault(root).id != keys->id) fail(QStringLiteral("Vault identity changed during removal; preserving its file"));
  for (const vault::Slot &slot : file.keySlots) factors::forget(slot.kind, slot.params);
  removeFile(path::join(root, vault::kVaultFile));
  vault::forgetRoots();
  catalog::releaseMemory(root);
  vault::lock(root);
  {
    std::lock_guard lock(passwordsMutex);
    passwords.erase(root);
  }
  emitProgress(root, QStringLiteral("decrypted"), done.load(), int(work.size()));
}

// One pass per root at a time; a request during a pass runs it once more afterwards, for files that arrived meanwhile.
std::map<QString, std::function<void()>> again;
void inBackground(const QString &root, std::function<void()> work) {
  {
    std::lock_guard lock(passMutex);
    if (running.contains(root)) {
      again[root] = std::move(work);
      return;
    }
    running.insert(root);
  }
  std::thread([root, work = std::move(work)]() mutable {
    setLowPriorityCurrentThread();
    for (;;) {
      try {
        work();
      } catch (const std::exception &error) {
        qWarning("Vault pass incomplete: %s", error.what());
        emitProgress(root, QStringLiteral("incomplete"), 0, 0, QString::fromUtf8(error.what()));
      }
      std::lock_guard lock(passMutex);
      if (!again.count(root) || !vault::keysFor(root)) {
        again.erase(root);
        running.remove(root);
        return;
      }
      work = std::move(again[root]);
      again.erase(root);
    }
  }).detach();
}

vault::Secret factorSecret(const vault::VaultFile &file, const QString &kind, const QString &reason, const QString &pin, const QString &recoveryKey) {
  if (kind == QLatin1String("recovery")) {
    auto key = vault::parseRecoveryKey(recoveryKey);
    if (!key) fail(QStringLiteral("This recovery key is mistyped"));
    return *key;
  }
  const vault::Slot *slot = slotFor(file, kind);
  if (!slot) fail(QStringLiteral("This second factor is not set up for this library"));
  return factors::secret(kind, slot->params, reason, pin);
}

QJsonObject finishUnlock(const QString &root, const vault::VaultFile &file, const vault::Secret &p, const QString &kind, const vault::Secret &s) {
  auto master = tryOpen(file, p, kind, s);
  if (!master) fail(QStringLiteral("Wrong password"));
  vault::unlock(root, keysFrom(*master, file.id));
  {
    std::lock_guard lock(passwordsMutex);
    passwords[root] = p;
  }
  // Leftovers of a crash, and files copied in while the root was locked, are encrypted now.
  inBackground(root, [root, decrypting = file.state == QLatin1String("decrypting")] { if (decrypting) runDecryption(root); else runEncryption(root); });
  return {{QStringLiteral("decoy"), false}};
}
}  // namespace

QJsonObject status(const QString &root) {
  std::optional<vault::VaultFile> file;
  try {
    file = vault::VaultFile::load(root);
  } catch (const Error &error) {
    return {{QStringLiteral("vault"), true}, {QStringLiteral("locked"), true}, {QStringLiteral("error"), error.message()}};
  }
  // A folder inside an encrypted root opened as a root of its own shows nothing.
  if (!file) return {{QStringLiteral("vault"), false}, {QStringLiteral("nested"), vault::isVaultPath(root)}};
  QJsonArray kinds, pinKinds;
  for (const vault::Slot &slot : file->keySlots) {
    if (slot.kind == QLatin1String("recovery")) continue;
    kinds.append(slot.kind);
    if (slot.kind == QLatin1String("tpm2") || slot.params.value(QLatin1String("pin")).toBool()) pinKinds.append(slot.kind);
  }
  bool resumable;
  {
    std::lock_guard lock(passwordsMutex);
    resumable = passwords.count(root) > 0;
  }
  return {{QStringLiteral("vault"), true}, {QStringLiteral("locked"), !vault::keysFor(root)}, {QStringLiteral("state"), file->state}, {QStringLiteral("factors"), kinds}, {QStringLiteral("pinFactors"), pinKinds}, {QStringLiteral("resumable"), resumable}};
}

bool backgroundPassActive(const QString &root) {
  std::lock_guard lock(passMutex);
  return running.contains(root);
}

QJsonObject factors() {
  QJsonArray out;
  for (const auto &item : factors::available()) out.append(QJsonObject{{QStringLiteral("kind"), item.kind}, {QStringLiteral("available"), item.available}, {QStringLiteral("system"), item.system}, {QStringLiteral("detail"), item.detail}});
  return {{QStringLiteral("factors"), out}};
}

QJsonObject setup(const QString &root, const QString &password, const QStringList &factorKinds, const QString &decoyPassword, const QString &reason, const QString &pin) {
  if (!isDir(root)) fail(QStringLiteral("%1 is not a directory").arg(root));
  FileAccess setupAccess({root}, true);
  auto setupLease = FileLock::acquireIn(*setupAccess.directory(root), QStringLiteral(".room237-vault-setup.lock"), std::chrono::milliseconds(0), std::chrono::milliseconds(5));
  if (!setupLease) fail(QStringLiteral("This root is being initialized"));
  vault::forgetRoots();
  if (vault::isVaultPath(root)) fail(QStringLiteral("This root is already inside an encrypted library"));
  for (const QString &album : album::walkAlbumPaths(root))
    if (entryExists(path::join(album, vault::kVaultFile))) fail(QStringLiteral("Initialize each root separately; nested vault found"));
  if (vault::VaultFile::load(root)) fail(QStringLiteral("This root is already encrypted"));
  for (const QString &album : albumsOf(root)) media_move::recoverAlbum(album);
  if (password.size() < 8) fail(QStringLiteral("The password is too short"));
  if (factorKinds.isEmpty()) fail(QStringLiteral("Choose a second factor"));
  if (!decoyPassword.isEmpty() && decoyPassword == password) fail(QStringLiteral("The decoy password must differ from the password"));
  vault::VaultFile file;
  file.id = toHex(vault::randomBytes(16));
  file.salt = vault::randomBytes(32);
  file.memoryKiB = kMemoryKiB;
  file.passes = kPasses;
  const vault::Secret master(vault::randomBytes(32));
  const vault::Secret p = vault::argon2id(password.toUtf8(), file.salt, file.memoryKiB, file.passes);
  for (const QString &kind : factorKinds) {
    factors::Enrollment enrolled = factors::enroll(kind, reason, pin);
    file.keySlots.push_back({kind, enrolled.params, vault::seal(vault::slotKey(p, kind, enrolled.secret), master.bytes(), file.id.toUtf8())});
  }
  const QString recovery = vault::newRecoveryKey();
  file.keySlots.push_back({QStringLiteral("recovery"), {}, vault::seal(vault::slotKey(p, QStringLiteral("recovery"), *vault::parseRecoveryKey(recovery)), master.bytes(), file.id.toUtf8())});
  // The decoy slot always holds 32 bytes, so the file does not tell whether a decoy password exists.
  file.decoyTag = decoyPassword.isEmpty() ? vault::randomBytes(32) : vault::decoyTag(vault::argon2id(decoyPassword.toUtf8(), file.salt, file.memoryKiB, file.passes));
  file.state = QStringLiteral("encrypting");
  catalog::releaseMemory(root);
  TempFile publication(root, QStringLiteral(".room237-vault-init-"));
  publication.write(QJsonDocument(file.toJson()).toJson(QJsonDocument::Indented));
  publication.sync();
  publication.persistNoClobber(path::join(root, vault::kVaultFile));
  vault::forgetRoots();
  vault::unlock(root, keysFrom(master, file.id));
  {
    std::lock_guard lock(passwordsMutex);
    passwords[root] = p;
  }
  preload::dropPreloadForPath(root);
  setupLease.reset();
  inBackground(root, [root] { runEncryption(root); });
  return {{QStringLiteral("recoveryKey"), recovery}};
}

QJsonObject unlock(const QString &root, const QString &password, const QString &kind, const QString &reason, const QString &pin, const QString &recoveryKey) {
  const vault::VaultFile file = loadVault(root);
  const vault::Secret p = vault::argon2id(password.toUtf8(), file.salt, file.memoryKiB, file.passes);
  const bool decoy = !file.decoyTag.isEmpty() && vault::decoyTag(p) == file.decoyTag;
  // The second factor is asked for either way, so the decoy password is indistinguishable at the screen.
  const vault::Secret s = factorSecret(file, kind, reason, pin, recoveryKey);
  if (decoy) return {{QStringLiteral("decoy"), true}};
  return finishUnlock(root, file, p, kind, s);
}

QJsonObject resume(const QString &root, const QString &kind, const QString &reason, const QString &pin) {
  const vault::VaultFile file = loadVault(root);
  vault::Secret p;
  {
    std::lock_guard lock(passwordsMutex);
    auto it = passwords.find(root);
    if (it == passwords.end()) fail(QStringLiteral("Enter the password"));
    p = it->second;
  }
  const vault::Secret s = factorSecret(file, kind, reason, pin, {});
  return finishUnlock(root, file, p, kind, s);
}

void lock(const QString &root) {
  preload::dropPreloadForPath(root);
  catalog::releaseMemory(root);
  vault::lock(root);
}

void lockAll() {
  QStringList roots;
  {
    std::lock_guard lock(passwordsMutex);
    for (const auto &[root, p] : passwords) roots << root;
  }
  for (const QString &root : roots) lock(root);
  vault::lockAll();
}

void encrypt(const QString &root) {
  if (!vault::keysFor(root)) fail(QStringLiteral("This library is locked"));
  inBackground(root, [root] { runEncryption(root); });
}

void decrypt(const QString &root) {
  if (!vault::keysFor(root)) fail(QStringLiteral("This library is locked"));
  inBackground(root, [root] { runDecryption(root); });
}

void addFactor(const QString &root, const QString &kind, const QString &reason, const QString &pin) {
  const auto keys = vault::keysFor(root);
  if (!keys) fail(QStringLiteral("This library is locked"));
  vault::VaultFile file = loadVault(root);
  vault::Secret p;
  {
    std::lock_guard lock(passwordsMutex);
    auto it = passwords.find(root);
    if (it == passwords.end()) fail(QStringLiteral("Enter the password"));
    p = it->second;
  }
  factors::Enrollment enrolled = factors::enroll(kind, reason, pin);
  // A factor of the same kind belonged to a key that no longer works (new fingerprints, another computer).
  std::vector<vault::Slot> kept;
  for (const vault::Slot &slot : file.keySlots)
    if (slot.kind != kind) kept.push_back(slot);
  kept.push_back({kind, enrolled.params, vault::seal(vault::slotKey(p, kind, enrolled.secret), keys->master.bytes(), file.id.toUtf8())});
  file.keySlots = kept;
  file.save(root);
}

bool verifyPassword(const QString &root, const QString &password) {
  const vault::VaultFile file = loadVault(root);
  const vault::Secret p = vault::argon2id(password.toUtf8(), file.salt, file.memoryKiB, file.passes);
  std::lock_guard lock(passwordsMutex);
  auto it = passwords.find(root);
  return it != passwords.end() && it->second.bytes() == p.bytes();
}

}  // namespace core::vault_ops
