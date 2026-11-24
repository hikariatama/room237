// Unit tests of the library core: on-disk formats shared with earlier releases, crash recovery of journaled media
// operations, update verification and unpacking.
#include "album.h"
#include "album_db.h"
#include "xattr.h"
#include "catalog.h"
#include "constants.h"
#include "entry.h"
#include "factors.h"
#include "fs.h"
#include "json.h"
#include "media_delete.h"
#include "media_move.h"
#include "metadata.h"
#include "settings.h"
#include "update.h"
#include "vault.h"
#include "vault_ops.h"
#include "host.h"
#include "maintenance.h"
#include "db.h"
#include "util.h"
#include "file_access.h"
#include "native_image.h"
#include "directory_move.h"
#include "album_tree.h"
#include "thumb.h"
#include <QProcess>
#include <sqlite3.h>
#include <QBuffer>
#include <QProcess>
#include <QProcessEnvironment>
#include <atomic>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>
#endif
#include "xattr.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QDirIterator>
#include <condition_variable>
#include <QtTest>
#include <zlib.h>

using namespace core;
using core::stat;

namespace {
QString write(const QString &dir, const QString &name, const QByteArray &bytes) {
  const QString p = path::join(dir, name);
  QFile file(p);
  if (!file.open(QIODevice::WriteOnly)) qFatal("cannot write %s", qPrintable(p));
  file.write(bytes);
  return p;
}
QString image(const QString &dir, const QString &name, QRgb color) {
  QImage picture(64, 48, QImage::Format_RGB32);
  picture.fill(color);
  const QString p = path::join(dir, name);
  picture.save(p, "PNG");
  return p;
}
QString real(const QTemporaryDir &dir) { return canonical(dir.path()); }

struct Checkpoint : Error {
  using Error::Error;
};

// A minimal ustar writer for archives the updater must refuse or accept.
QByteArray tarEntry(const QByteArray &name, char type, const QByteArray &body = {}, const QByteArray &link = {}) {
  QByteArray header(512, '\0');
  memcpy(header.data(), name.constData(), size_t(std::min<qsizetype>(name.size(), 100)));
  memcpy(header.data() + 100, type == '5' ? "0000755" : "0000644", 7);
  memcpy(header.data() + 108, "0000000", 7);
  memcpy(header.data() + 116, "0000000", 7);
  const QByteArray size = QByteArray::number(body.size(), 8).rightJustified(11, '0');
  memcpy(header.data() + 124, size.constData(), 11);
  memcpy(header.data() + 136, "00000000000", 11);
  header[156] = type;
  memcpy(header.data() + 157, link.constData(), size_t(std::min<qsizetype>(link.size(), 100)));
  memcpy(header.data() + 257, "ustar\0" "00", 8);
  memset(header.data() + 148, ' ', 8);
  unsigned sum = 0;
  for (char c : header) sum += uchar(c);
  const QByteArray checksum = QByteArray::number(sum, 8).rightJustified(6, '0');
  memcpy(header.data() + 148, checksum.constData(), 6);
  header[154] = '\0';
  QByteArray out = header + body;
  out += QByteArray((512 - body.size() % 512) % 512, '\0');
  return out;
}
void gzipTo(const QString &target, const QByteArray &tar) {
  gzFile file = gzopen(QFile::encodeName(target).constData(), "wb");
  gzwrite(file, tar.constData(), unsigned(tar.size()));
  gzclose(file);
}

// Collects "vault-progress" events of the background passes.
struct ProgressWatch {
  std::mutex mutex;
  std::condition_variable changed;
  QStringList phases;
  QString error;
  ProgressWatch() {
    host::setEventSink([this](const QString &name, const QJsonValue &payload) {
      if (name != QLatin1String("vault-progress")) return;
      std::lock_guard lock(mutex);
      phases << payload[QLatin1String("phase")].toString();
      if (!payload[QLatin1String("error")].toString().isEmpty()) error = payload[QLatin1String("error")].toString();
      changed.notify_all();
    });
  }
  ~ProgressWatch() { host::setEventSink({}); }
  // Waits for one of the final phases and returns it.
  QString wait(const QStringList &finals) {
    std::unique_lock lock(mutex);
    QString found;
    changed.wait_for(lock, std::chrono::seconds(60), [&] {
      for (const QString &phase : phases)
        if (finals.contains(phase)) found = phase;
      return !found.isEmpty();
    });
    phases.clear();
    return found;
  }
};

// Paths under `root` whose name or bytes contain `needle`, for checking that nothing plain is left.
QStringList leaks(const QString &root, const QByteArray &needle) {
  QStringList out;
  QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString file = it.next();
    if (file.toUtf8().contains(needle) || (QFileInfo(file).isFile() && readAll(file).contains(needle))) out << file;
  }
  return out;
}
}  // namespace

namespace {
bool syntheticLink(const QString &target, const QString &link, bool directory = false) {
#ifdef Q_OS_WIN
  return CreateSymbolicLinkW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(link).utf16()),
      reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(target).utf16()), (directory ? 1 : 0) | 2);
#else
  Q_UNUSED(directory)
  return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0;
#endif
}
// The wrapper has process lifetime: background core connections must never retain a stack-allocated VFS.
struct TempIoAudit {
  sqlite3_vfs wrapped{}, *original = nullptr;
  std::atomic<int> temporary{0};
  static TempIoAudit &instance() { static auto *audit = new TempIoAudit; return *audit; }
  TempIoAudit() {
    original = sqlite3_vfs_find(nullptr);
    wrapped = *original;
    wrapped.zName = "room237-security-temp-audit";
    wrapped.xOpen = [](sqlite3_vfs *, const char *name, sqlite3_file *file, int flags, int *out) {
      auto &audit = instance();
      if (flags & (SQLITE_OPEN_TEMP_DB | SQLITE_OPEN_TRANSIENT_DB | SQLITE_OPEN_TEMP_JOURNAL | SQLITE_OPEN_SUBJOURNAL)) ++audit.temporary;
      return audit.original->xOpen(audit.original, name, file, flags, out);
    };
  }
};
struct AuditScope {
  AuditScope() { sqlite3_vfs_register(&TempIoAudit::instance().wrapped, 1); TempIoAudit::instance().temporary = 0; }
  ~AuditScope() { sqlite3_vfs_register(TempIoAudit::instance().original, 1); }
};
}  // namespace

class CoreTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void initTestCase() {
    profile_.reset(new QTemporaryDir);
    SettingsStore::load(profile_->path());
  }

  void reconcileAppliesSmallChangesFileByFile() {
    QTemporaryDir fixture;
    const QString root = real(fixture), album = path::join(root, "Album");
    createDir(album);
    for (int i = 0; i < 6; i++) image(album, QStringLiteral("p%1.png").arg(i), qRgb(i * 30, 40, 90));
    catalog::initialize(root);
    QCOMPARE(catalog::scanAlbum(root, album), 6);
    QVERIFY(!catalog::reconcileAlbum(root, "Album"));
    const auto names = [&] {
      catalog::PageQuery query;
      query.rootDir = root;
      query.albumId = QStringLiteral("Album");
      query.sortKey = QStringLiteral("name");
      query.sortDir = QStringLiteral("asc");
      QStringList out;
      for (const catalog::PageItem &item : catalog::queryPage(query).items) out << item.media.name;
      return out;
    };
    image(album, "new1.png", qRgb(1, 2, 3));
    image(album, "new2.png", qRgb(4, 5, 6));
    removeFile(path::join(album, "p0.png"));
    QVERIFY(catalog::reconcileAlbum(root, "Album"));
    const QStringList incremental = names();
    QCOMPARE(incremental, (QStringList{"new1.png", "new2.png", "p1.png", "p2.png", "p3.png", "p4.png", "p5.png"}));
    // The same listing as a full scan, and a folder that matches its catalog afterwards.
    QVERIFY(!catalog::reconcileAlbum(root, "Album"));
    QCOMPARE(catalog::scanAlbum(root, album), 7);
    QCOMPARE(names(), incremental);
    // An album listing after a change outside the app lists the changed files alone, with the same result.
    image(album, "new3.png", qRgb(7, 8, 9));
    removeFile(path::join(album, "p1.png"));
    QCOMPARE(catalog::scanAlbumIfNeeded(root, album, false), 7);
    const QStringList listed = names();
    QCOMPARE(listed, (QStringList{"new1.png", "new2.png", "new3.png", "p2.png", "p3.png", "p4.png", "p5.png"}));
    QCOMPARE(catalog::scanAlbumIfNeeded(root, album, false), 7);
    QCOMPARE(catalog::scanAlbum(root, album), 7);
    QCOMPARE(names(), listed);
  }
  void replacedAlbumFolderIsListed() {
    QTemporaryDir fixture;
    const QString base = real(fixture), root = path::join(base, "lib"), album = path::join(root, "Open");
    createDirs(album);
    for (int i = 0; i < 3; i++) image(album, QStringLiteral("o%1.png").arg(i), qRgb(i * 60, 10, 10));
    catalog::initialize(root);
    QCOMPARE(catalog::scanAlbum(root, album), 3);
    QVERIFY(::rename(QFile::encodeName(album).constData(), QFile::encodeName(path::join(base, "aside")).constData()) == 0);
    createDir(album);
    image(album, "n0.png", qRgb(1, 2, 3));
    image(album, "n1.png", qRgb(4, 5, 6));
    QVERIFY(catalog::reconcileAlbum(root, "Open"));
    catalog::PageQuery query;
    query.rootDir = root;
    query.albumId = QStringLiteral("Open");
    query.sortKey = QStringLiteral("name");
    query.sortDir = QStringLiteral("asc");
    QStringList names;
    for (const catalog::PageItem &item : catalog::getMediaPage(query).items) names << item.media.name;
    QCOMPARE(names, (QStringList{"n0.png", "n1.png"}));
  }
  void reconcileKeepsFavoriteOfRenamedFile() {
    QTemporaryDir fixture;
    const QString root = real(fixture), album = path::join(root, "Album");
    createDir(album);
    for (int i = 0; i < 4; i++) image(album, QStringLiteral("p%1.png").arg(i), qRgb(i * 50, 90, 40));
    catalog::initialize(root);
    QCOMPARE(catalog::scanAlbum(root, album), 4);
    metadata::setMediaFavorite(path::join(album, "p1.png"), true);
    QVERIFY(::rename(QFile::encodeName(path::join(album, "p1.png")).constData(), QFile::encodeName(path::join(album, "renamed.png")).constData()) == 0);
    QVERIFY(catalog::reconcileAlbum(root, "Album"));
    catalog::PageQuery query;
    query.rootDir = root;
    query.albumId = QStringLiteral("Album");
    query.sortKey = QStringLiteral("name");
    query.sortDir = QStringLiteral("asc");
    QStringList favorites;
    for (const catalog::PageItem &item : catalog::queryPage(query).items)
      if (item.media.favorite.value_or(false)) favorites << item.media.name;
    QCOMPARE(favorites, QStringList{"renamed.png"});
  }
  void moveValidationIgnoresNewPreviews() {
    QTemporaryDir fixture;
    const QString album = path::join(real(fixture), "Album");
    createDir(album);
    const QString file = image(album, "p.png", qRgb(10, 20, 30));
    const QString listed = metadata::mediaRevision(file);
    createDir(path::join(album, ".room237-thumb"));
    write(path::join(album, ".room237-thumb"), "p.png.webp", "preview built after the listing");
    QVERIFY(metadata::mediaRevision(file) != listed);
    metadata::validateMediaRevisions({metadata::ObservedMedia{file, listed}});
    image(album, "p.png", qRgb(200, 20, 30));
    write(album, "p.png", QByteArray(5000, 'x'));
    QVERIFY_THROWS_EXCEPTION(Error, metadata::validateMediaRevisions({metadata::ObservedMedia{file, listed}}));
  }
  void albumRenameChangesOnlyTheCase() {
    QTemporaryDir fixture;
    const QString root = real(fixture);
    createDir(path::join(root, "Trips"));
    image(path::join(root, "Trips"), "p.png", qRgb(1, 2, 3));
    catalog::initialize(root);
    const album::RenamedAlbumResult result = album::renameAlbum(root, "Trips", "trips");
    QCOMPARE(result.newRelativePath, QStringLiteral("trips"));
    QCOMPARE(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).filter(QStringLiteral("rips")), QStringList{"trips"});
    QVERIFY(isFile(path::join(path::join(root, "trips"), "p.png")));
  }
  void copiedLibraryKeepsPreviewAndMetadata() {
#ifdef Q_OS_WIN
    QSKIP("cp -Rp is not available");
#else
    QTemporaryDir fixture;
    const QString root = real(fixture), album = path::join(root, "Album"), copy = path::join(root, "Copy");
    createDir(album);
    const QString file = image(album, "p.png", qRgb(10, 200, 30));
    catalog::initialize(root);
    thumb::ensureThumb(file, path::join(album, thumb::kThumbDir));
    metadata::getFileMetadataCached(file);
    QVERIFY(metadata::metadataCacheCurrent(file, *metadata::readFileEntry(file)));
    QCOMPARE(QProcess::execute("cp", {"-Rp", album, copy}), 0);
    const QString copied = path::join(copy, "p.png"), preview = thumb::thumbPath(copied, path::join(copy, thumb::kThumbDir));
    const auto previewBefore = stat(preview).mtimeNs;
    QVERIFY(!metadata::metadataCacheCurrent(copied, *metadata::readFileEntry(copied)));
    thumb::ensureThumb(copied, path::join(copy, thumb::kThumbDir));
    QCOMPARE(stat(preview).mtimeNs, previewBefore);
    QVERIFY(metadata::metadataCacheCurrent(copied, *metadata::readFileEntry(copied)));
#endif
  }
  void appleDoubleFilesAreNotMedia() {
    QVERIFY(!constants::isMedia("/library/Album/._photo.jpg"));
    QVERIFY(!constants::isMedia("._photo.jpg"));
    QVERIFY(constants::isMedia("/library/Album/photo.jpg"));
    QVERIFY(constants::isMedia("/library/Album/.hidden.jpg"));
  }
  void pendingRecoveryRejectsServiceSymlinks() {
    QTemporaryDir fixture;
    const QString base = real(fixture), root = path::join(base, "library"), victim = path::join(base, "victim");
    createDir(root); createDir(victim); catalog::initialize(root);
    const QString sentinel = write(victim, "ordinary.txt", "not an album");
    const QString pending = path::join(path::join(root, catalog::kDirectory), "pending");
    if (!syntheticLink(victim, pending, true)) QSKIP("Synthetic directory symlinks unavailable");
    QVERIFY_THROWS_EXCEPTION(Error, catalog::favoriteCounts(root));
    QCOMPARE(readAll(sentinel), QByteArray("not an album"));
    const QString album = path::join(root, "Album"); createDir(album);
    write(album, "photo.jpg", "synthetic");
    const QString victimMarker = write(victim, sha256Hex("Album"), "keep marker target");
    const QString catalogDir = path::join(root, catalog::kDirectory);
    write(catalogDir, "index.sqlite", "not sqlite");  // Force the derived-index failure / pending publication path.
    catalog::publish(album, "photo.jpg", FileMetaEntry{});
    QCOMPARE(readAll(victimMarker), QByteArray("keep marker target"));
    removeFile(path::join(catalogDir, "index.sqlite"));
    catalog::initialize(root);
    Directory(path::parent(pending)).removeEntry("pending");
    createDir(pending);
    write(pending, "ordinary.txt", "not an album");
    write(pending, sha256Hex("Missing"), "Missing");
    write(pending, sha256Hex("Other"), "Wrong digest");
    const QString linked = path::join(pending, sha256Hex("Linked"));
    QVERIFY(syntheticLink(sentinel, linked));
    catalog::favoriteCounts(root);
    QVERIFY(!entryExists(path::join(pending, sha256Hex("Missing"))));
    QVERIFY(entryExists(path::join(pending, "ordinary.txt")));
    QVERIFY(entryExists(path::join(pending, sha256Hex("Other"))));
    QVERIFY(lstat(linked).isSymlink);
    QCOMPARE(readAll(sentinel), QByteArray("not an album"));
  }

  void thumbnailMaintenanceRejectsRedirectedCache() {
    QTemporaryDir fixture;
    const QString base = real(fixture), root = path::join(base, "library"), album = path::join(root, "Album"), victim = path::join(base, "victim");
    createDir(root); createDir(album); createDir(victim); createDir(path::join(victim, "nested"));
    const QString sentinel = write(path::join(victim, "nested"), "keep.txt", "keep");
    const QString cache = path::join(album, ".room237-thumb");
    if (!syntheticLink(victim, cache, true)) QSKIP("Synthetic directory symlinks unavailable");
    QVERIFY_THROWS_EXCEPTION(Error, maintenance::rebuildThumbnails(root));
    QCOMPARE(readAll(sentinel), QByteArray("keep"));
    maintenance::clearRoom237Artifacts(root);
    QCOMPARE(readAll(sentinel), QByteArray("keep"));
    QVERIFY(lstat(cache).isSymlink);
    Directory(album).removeEntry(".room237-thumb"); createDir(cache); createDir(path::join(cache, "nested"));
    write(path::join(cache, "nested"), "stale.webp", "cache");
    maintenance::rebuildThumbnails(root);
    QVERIFY(!entryExists(path::join(cache, "nested")));
  }

  void migrationUsesRegularSourcesAndExclusiveOpenTemporaries() {
    QTemporaryDir fixture;
    const QString base = real(fixture), profile = path::join(base, "profile"), legacy = path::join(base, "legacy");
    createDir(profile); createDir(legacy);
    const QString victim = write(base, "victim.json", "{\"outside\":true}");
    const QString settings = path::join(legacy, "settings.json");
    if (!syntheticLink(victim, settings)) QSKIP("Synthetic file symlinks unavailable");
    QVERIFY_THROWS_EXCEPTION(Error, maintenance::migrateProfile(profile, legacy));
    QVERIFY(!entryExists(path::join(profile, "settings.json")));
    QVERIFY(!entryExists(path::join(profile, "migration-tauri.json")));
    removeFile(settings); write(legacy, "settings.json", "{\"privacy\":true}");
    for (const QString &name : {QStringLiteral("settings.migrating"), QStringLiteral("ui-state.migrating"), QStringLiteral("migration-tauri.migrating")})
      QVERIFY(syntheticLink(victim, path::join(profile, name)));
    write(legacy, "state.json", "{\"room237-storage\":{\"columns\":4}}");
    const auto outcome = maintenance::migrateProfile(profile, legacy);
    QVERIFY(outcome.settings && outcome.state);
    QCOMPARE(readAll(path::join(profile, "settings.json")), QByteArray("{\"privacy\":true}"));
    QCOMPARE(readAll(victim), QByteArray("{\"outside\":true}"));
    QCOMPARE(readAll(settings), QByteArray("{\"privacy\":true}"));
    const auto again = maintenance::migrateProfile(profile, legacy);
    QCOMPARE(again.state, false);
  }

  void directoryCapabilitiesRefuseSubstitution() {
    QTemporaryDir fixture;
    const QString base = real(fixture), original = path::join(base, "original"), victim = path::join(base, "victim");
    createDir(original); createDir(victim); write(original, "source", "original");
    const QString sentinel = write(victim, "source", "victim");
    Directory parent(base);
    Directory admitted = parent.childForRemoval("original");
    const QString id = admitted.identity();
#ifdef Q_OS_WIN
    // The directory and all ancestors are pinned against rename/reparse changes on Windows.
    QVERIFY(!QDir().rename(original, path::join(base, "retired")));
#else
    QVERIFY(QDir().rename(original, path::join(base, "retired")));
    QVERIFY(syntheticLink(victim, original, true));
    QVERIFY_THROWS_EXCEPTION(Error, admitted.writeAtomic("new-file", "bytes"));
    QVERIFY_THROWS_EXCEPTION(Error, parent.removeEntry("original", id, &admitted));
    QCOMPARE(readAll(path::join(path::join(base, "retired"), "source")), QByteArray("original"));
#endif
    QCOMPARE(readAll(sentinel), QByteArray("victim"));
    QVERIFY(!entryExists(path::join(victim, "new-file")));
  }

  void atomicPublicationClaimsAndPreservesExchangedTemporary() {
    QTemporaryDir fixture;
    const QString base = real(fixture);
    write(base, "target", "previous");
    const QString victim = write(base, "victim", "outside");
    Directory directory(base);
#ifndef Q_OS_WIN
    QVERIFY_THROWS_EXCEPTION(Error, directory.writeAtomic("target", "new bytes", [&](const QString &temporary) {
      QVERIFY(QFile::rename(path::join(base, temporary), path::join(base, "retired")));
      QVERIFY(syntheticLink(victim, path::join(base, temporary)));
    }));
    QCOMPARE(readAll(path::join(base, "target")), QByteArray("previous"));
    QCOMPARE(readAll(path::join(base, "retired")), QByteArray("new bytes"));
    bool preserved = false;
    for (const auto &entry : directory.entries()) if (entry.name.startsWith(".room237-write-claim-")) {
      auto staging = directory.child(entry.name);
      preserved |= staging.info("entry").isSymlink;
    }
    QVERIFY(preserved);
#else
    directory.writeAtomic("target", "new bytes", [&](const QString &temporary) {
      QVERIFY(!QFile::rename(path::join(base, temporary), path::join(base, "retired")));
    });
    QCOMPARE(readAll(path::join(base, "target")), QByteArray("new bytes"));
#endif
    QCOMPARE(readAll(victim), QByteArray("outside"));
  }

  void admittedSourceReadersKeepOpenedObject() {
    QTemporaryDir fixture;
    const QString base = real(fixture), root = path::join(base, "library"); createDir(root);
    const QString source = image(root, "photo.png", qRgb(12, 34, 56));
    const QString victim = image(base, "victim.png", qRgb(99, 88, 77));
    const QByteArray original = readAll(source), outside = readAll(victim);
    FileAccess access({root});
    auto pinned = access.pin(source);
#ifndef Q_OS_WIN
    QVERIFY(QFile::rename(source, path::join(root, "retired.png")));
    QVERIFY(syntheticLink(victim, source));
    QCOMPARE(readAll(source), original);
    QFile reader; QVERIFY(openFile(reader, source, QIODevice::ReadOnly)); QCOMPARE(reader.readAll(), original);
#ifdef Q_OS_MACOS
    const auto dimensions = native_image::dimensions(decoderPath(source));
    QVERIFY(dimensions); QCOMPARE(dimensions->first, quint32(64)); QCOMPARE(dimensions->second, quint32(48));
#endif
    xattr::set(source, "user.room237.security_test", "opened object");
    QCOMPARE(xattr::get(source, "user.room237.security_test").value, std::optional<QByteArray>("opened object"));
    QVERIFY(!xattr::get(victim, "user.room237.security_test").value);
#else
    QVERIFY(!QFile::rename(source, path::join(root, "retired.png")));
    QCOMPARE(readAll(source), original);
#endif
    const auto child = runProcess(QCoreApplication::applicationFilePath(), {"--security-read-file", decoderPath(source)}, 5000);
    QVERIFY(child.success); QCOMPARE(child.stdoutBytes, original);
    QCOMPARE(readAll(victim), outside);
  }

  void sqliteConnectionsKeepAdmittedParents() {
    QTemporaryDir fixture;
    const QString base = real(fixture), root = path::join(base, "root"), folder = path::join(root, "metadata"), victim = path::join(base, "victim");
    createDir(root); createDir(folder); createDir(victim);
    const QString sentinel = write(victim, "album.sqlite", "outside");
    FileAccess access({root});
    db::Connection conn(path::join(folder, "album.sqlite"));
    conn.executeBatch("CREATE TABLE example(value TEXT); INSERT INTO example VALUES('before');");
#ifndef Q_OS_WIN
    QVERIFY(QDir().rename(folder, path::join(root, "retired")));
    QVERIFY(syntheticLink(victim, folder, true));
    QVERIFY_THROWS_EXCEPTION(Error, conn.executeBatch("INSERT INTO example VALUES('after');"));
#else
    QVERIFY(!QDir().rename(folder, path::join(root, "retired")));
    conn.executeBatch("INSERT INTO example VALUES('after');");
#endif
    QCOMPARE(readAll(sentinel), QByteArray("outside"));
  }

  void macSystemAliasesRemainUsable() {
#ifdef Q_OS_MACOS
    for (const QString &alias : {QStringLiteral("/tmp"), QStringLiteral("/var")})
      QCOMPARE(Directory(alias).identity(), directoryId(canonical(alias)));
#else
    QSKIP("macOS system directory aliases");
#endif
  }

  void directoryTransactionsRecoverEveryDurablePhase_data() {
    QTest::addColumn<QString>("phase");
    for (const char *phase : {"directory_intent", "directory_before_claim", "directory_claimed", "directory_before_publication", "directory_published", "directory_before_rollback", "directory_rolled_back"})
      QTest::newRow(phase) << QString::fromLatin1(phase);
  }
  void directoryTransactionsRecoverEveryDurablePhase() {
    QFETCH(QString, phase);
    QTemporaryDir fixture; const QString root = real(fixture), source = path::join(root, "Album"), target = path::join(root, "Renamed");
    createDir(source); write(source, "original", "media bytes");
    const QString identity = directoryId(source);
    bool interrupted = false;
    {
      FileAccess access({root}); auto library = access.directory(root); auto admitted = library->childForRemoval("Album");
      auto tree = album_tree::write(root);
      try {
        directory_move::move(root, *library, "Album", "Renamed", admitted, [&](const QString &at) {
          if (phase.contains("rollback") || phase == "directory_rolled_back") if (at == "directory_before_publication") { createDir(target); write(target, "new-occupant", "keep"); }
          if (at == phase) { interrupted = true; throw Checkpoint("Synthetic directory process death"); }
        });
      } catch (const Checkpoint &) {}
    }  // Drop every operation handle, as on restart.
    QVERIFY(interrupted);
    directory_move::recover(root); directory_move::recover(root);
    const QString restored = phase == "directory_published" ? target : source;
    QCOMPARE(directoryId(restored), identity); QCOMPARE(readAll(path::join(restored, "original")), QByteArray("media bytes"));
    if (phase.contains("rollback") || phase == "directory_rolled_back") QCOMPARE(readAll(path::join(target, "new-occupant")), QByteArray("keep"));
    QCOMPARE(listDir(path::join(root, ".room237-transactions")).size(), 0);
  }

  void directoryRecoveryItselfCanRestart_data() {
    QTest::addColumn<QString>("phase");
    for (const char *phase : {"directory_recovery_loaded", "directory_recovery_before_restore", "directory_recovery_restored", "directory_recovery_before_cleanup"}) QTest::newRow(phase) << QString::fromLatin1(phase);
  }
  void directoryRecoveryItselfCanRestart() {
    QFETCH(QString, phase);
    QTemporaryDir fixture; const QString root = real(fixture); createDir(path::join(root, "Album")); write(path::join(root, "Album"), "original", "bytes");
    {
      FileAccess access({root}); auto library = access.directory(root); auto admitted = library->childForRemoval("Album"); auto tree = album_tree::write(root);
      QVERIFY_THROWS_EXCEPTION(Checkpoint, directory_move::move(root, *library, "Album", "Renamed", admitted, [](const QString &at) { if (at == "directory_claimed") throw Checkpoint("Synthetic process death"); }));
    }
    bool interrupted = false;
    QVERIFY_THROWS_EXCEPTION(Checkpoint, directory_move::recover(root, [&](const QString &at) { if (at == phase) { interrupted = true; throw Checkpoint("Synthetic recovery death"); } }));
    QVERIFY(interrupted); directory_move::recover(root);
    QCOMPARE(readAll(path::join(path::join(root, "Album"), "original")), QByteArray("bytes"));
    QCOMPARE(listDir(path::join(root, ".room237-transactions")).size(), 0);
  }

  void directoryRecoveryRejectsForgedPathsAndMembership_data() {
    QTest::addColumn<QString>("mutation");
    for (const char *field : {"malformed", "absolute", "parent-escape", "root_id", "source_id", "source_parent_id", "target_parent_id", "transaction", "symlink", "valid-unstarted"}) QTest::newRow(field) << QString::fromLatin1(field);
  }
  void directoryRecoveryRejectsForgedPathsAndMembership() {
    QFETCH(QString, mutation);
    QTemporaryDir fixture; const QString base = real(fixture), root = path::join(base, "library"), outside = path::join(base, "victim");
    createDir(root); createDir(outside); const QString sentinel = write(outside, "keep", "outside"); createDir(path::join(root, "Album")); write(path::join(root, "Album"), "original", "inside");
    {
      FileAccess access({root}); auto library = access.directory(root); auto admitted = library->childForRemoval("Album"); auto tree = album_tree::write(root);
      QVERIFY_THROWS_EXCEPTION(Checkpoint, directory_move::move(root, *library, "Album", "Renamed", admitted, [](const QString &at) { if (at == "directory_intent") throw Checkpoint("Synthetic process death"); }));
    }
    const QString service = path::join(root, ".room237-transactions"), stage = path::join(service, listDir(service).front()), journal = path::join(stage, "intent.json");
    auto object = QJsonDocument::fromJson(readAll(journal)).object();
    if (mutation == "absolute") object["source"] = outside;
    else if (mutation == "parent-escape") object["target"] = QString("../victim");
    else if (mutation == "valid-unstarted") object["target"] = QString("Unrequested");
    else if (mutation == "symlink") { if (!syntheticLink(outside, path::join(stage, "entry"), true)) QSKIP("Synthetic symlinks unavailable"); }
    else if (mutation != "malformed") object[mutation] = QString("forged");
    write(stage, "intent.json", mutation == "malformed" ? QByteArray("{") : QJsonDocument(object).toJson());
    if (mutation == "valid-unstarted") directory_move::recover(root);
    else { QVERIFY_THROWS_EXCEPTION(Error, directory_move::recover(root)); QVERIFY(isFile(journal)); }
    QCOMPARE(readAll(sentinel), QByteArray("outside")); QCOMPARE(readAll(path::join(path::join(root, "Album"), "original")), QByteArray("inside"));
    QVERIFY(!entryExists(path::join(root, "Unrequested"))); QVERIFY(!entryExists(path::join(root, "Renamed")));
  }

  void directoryTransactionsPreserveSubstitutedEntries_data() {
    QTest::addColumn<QString>("race");
    for (const char *race : {"source", "source-parent", "target-parent", "rollback-collision"}) QTest::newRow(race) << QString::fromLatin1(race);
  }
  void directoryTransactionsPreserveSubstitutedEntries() {
    QFETCH(QString, race);
    QTemporaryDir fixture; const QString base = real(fixture), root = path::join(base, "library"), outside = path::join(base, "victim");
    createDir(root); createDir(outside); const QString sentinel = write(outside, "keep", "outside");
    const QString parent = path::join(root, "Parent"), destination = path::join(root, "Destination"); createDir(parent); createDir(destination);
    createDir(path::join(parent, "Album")); write(path::join(parent, "Album"), "original", "inside");
    bool injected = false;
    {
      FileAccess access({root}); auto library = access.directory(root); auto sourceParent = library->child("Parent"); auto admitted = sourceParent.childForRemoval("Album"); auto tree = album_tree::write(root);
      bool refused = false;
      try { directory_move::move(root, *library, path::join("Parent", "Album"), path::join("Destination", "Album"), admitted, [&](const QString &at) {
        if (race == "rollback-collision" && at == "directory_before_publication") { createDir(path::join(destination, "Album")); write(path::join(destination, "Album"), "foreign", "target"); }
        if (race == "rollback-collision" && at == "directory_before_rollback") { createDir(path::join(parent, "Album")); write(path::join(parent, "Album"), "foreign", "source"); injected = true; return; }
        if ((race == "source" || race == "source-parent") && at != "directory_before_claim") return;
        if (race == "target-parent" && at != "directory_before_publication") return;
        if (race == "rollback-collision" || injected) return;
        injected = true;
        const QString path = race == "source" ? core::path::join(parent, "Album") : race == "source-parent" ? parent : destination;
#ifndef Q_OS_WIN
        if (!QDir().rename(path, path + ".retired") || !syntheticLink(outside, path, true)) fail(QStringLiteral("Synthetic directory exchange failed"));
#else
        if (QDir().rename(path, path + ".retired")) fail(QStringLiteral("Windows directory capability did not pin its entry"));
        throw Checkpoint("Synthetic interruption after blocked exchange");
#endif
      }); } catch (const Error &) { refused = true; }
      QVERIFY(refused);
    }
    QVERIFY(injected); QCOMPARE(readAll(sentinel), QByteArray("outside"));
#ifndef Q_OS_WIN
    QVERIFY_THROWS_EXCEPTION(Error, directory_move::recover(root));
#endif
    bool retained = false;
    QDirIterator iterator(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) if (readAll(iterator.next()) == QByteArray("inside")) retained = true;
    QVERIFY(retained);
    if (race == "rollback-collision") { QCOMPARE(readAll(path::join(path::join(parent, "Album"), "foreign")), QByteArray("source")); QCOMPARE(readAll(path::join(path::join(destination, "Album"), "foreign")), QByteArray("target")); }
  }

  void movesPreserveVictimsAcrossClaimAndDestinationRaces_data() {
    QTest::addColumn<QString>("phase");
    QTest::newRow("source-parent") << QString("before_rename");
    QTest::newRow("source-entry") << QString("before_move_claim");
    QTest::newRow("destination-parent") << QString("before_move_publication");
    QTest::newRow("private-publication-entry") << QString("before_private_move_publication");
    QTest::newRow("private-rollback-entry") << QString("before_private_rollback_publication");
    QTest::newRow("rollback-entry") << QString("before_rollback_claim");
    QTest::newRow("rollback-collision") << QString("before_rollback_publication");
  }
  void movesPreserveVictimsAcrossClaimAndDestinationRaces() {
    QFETCH(QString, phase);
    QTemporaryDir fixture;
    const QString base = real(fixture), source = path::join(base, "source"), target = path::join(base, "target"), victim = path::join(base, "victim");
    createDir(source); createDir(target); createDir(victim);
    const QString original = image(source, "photo.png", qRgb(1, 2, 3)), outside = image(victim, "photo.png", qRgb(4, 5, 6));
    const QByteArray originalBytes = readAll(original), outsideBytes = readAll(outside);
    metadata::setMediaFavorite(original, true);  // Exercise metadata rollback, not just the rename fast path.
    bool injected = false;
    auto hook = [&](const QString &at, const QString &, const QString &) {
      if (phase.contains("rollback") && at == "before_metadata") fail(QStringLiteral("Synthetic metadata failure"));
      if (at != phase || injected) return;
      injected = true;
#ifndef Q_OS_WIN
      QString from, replacement;
      bool directory = false;
      if (phase == "before_rename") { from = source; replacement = victim; directory = true; }
      else if (phase == "before_move_publication") { from = target; replacement = victim; directory = true; }
      else if (phase == "before_rollback_publication") { write(source, "photo.png", "new source occupant"); return; }
      else if (phase.startsWith("before_private_")) {
        QDirIterator iterator(source, {"entry"}, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
        if (!iterator.hasNext()) fail(QStringLiteral("Missing synthetic move claim"));
        from = iterator.next(); replacement = outside;
      }
      else { from = path::join(phase == "before_rollback_claim" ? target : source, "photo.png"); replacement = outside; }
      if (!(directory ? QDir().rename(from, from + ".retired") : QFile::rename(from, from + ".retired")) || !syntheticLink(replacement, from, directory)) fail(QStringLiteral("Synthetic exchange failed"));
#else
      const QString from = phase == "before_move_publication" ? target : phase == "before_rename" ? source : path::join(phase == "before_rollback_claim" ? target : source, "photo.png");
      if (phase == "before_rollback_publication") { write(source, "photo.png", "new source occupant"); return; }
      if (QDir().rename(from, from + ".retired") || QFile::rename(from, from + ".retired")) fail(QStringLiteral("Windows admission did not pin the entry"));
      fail(QStringLiteral("Synthetic interruption after blocked exchange"));
#endif
    };
    bool refused = false;
    try { refused = !album::moveMediaBatchWithHook(source, target, {"photo.png"}, true, hook).empty(); }
    catch (const Error &) { refused = true; }
    QVERIFY(injected); QVERIFY(refused);
    QCOMPARE(readAll(outside), outsideBytes);
    QStringList roots{base};
    bool retainedOriginal = false;
    QDirIterator files(base, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) { const auto path = files.next(); if (readAll(path) == originalBytes) retainedOriginal = true; }
    QVERIFY(retainedOriginal);
    if (phase == "before_rollback_publication") QCOMPARE(readAll(original), QByteArray("new source occupant"));
  }

  void vaultConversionsPreserveVictimsAcrossRaces_data() {
    QTest::addColumn<QString>("phase"); QTest::addColumn<bool>("parent");
    QTest::newRow("parent-before-open") << QString("before_source_open") << true;
    QTest::newRow("source-before-open") << QString("before_source_open") << false;
    QTest::newRow("temporary-before-publication") << QString("before_publication") << false;
    QTest::newRow("private-temporary-before-publication") << QString("before_private_publication") << false;
    QTest::newRow("recovered-temporary-before-publication") << QString("before_recovery_publication") << false;
    QTest::newRow("source-before-claim") << QString("before_source_claim") << false;
    QTest::newRow("claimed-before-unlink") << QString("before_claimed_unlink") << false;
  }
  void vaultConversionsPreserveVictimsAcrossRaces() {
    QFETCH(QString, phase); QFETCH(bool, parent);
    QTemporaryDir fixture;
    const QString base = real(fixture), root = path::join(base, "vault"), album = path::join(root, "Album"), victim = path::join(base, "victim");
    createDir(root); createDir(album); createDir(victim);
    const QString original = image(album, "photo.png", qRgb(1, 2, 3)), outside = image(victim, "photo.png", qRgb(4, 5, 6));
    const QByteArray originalBytes = readAll(original), outsideBytes = readAll(outside);
    vault::VaultFile file; file.id = "security-race-vault"; file.salt = vault::randomBytes(32); file.state = "encrypting"; file.save(root);
    vault::Keys keys; keys.id = file.id; keys.master = vault::Secret(vault::randomBytes(32)); keys.rows = vault::derive(keys.master, "rows"); keys.names = vault::derive(keys.master, "names");
    vault::unlock(root, keys);
    auto unlocked = vault::keysFor(root);
    FileAccess access(unlocked->filesystem);
    std::atomic<bool> injected = false;
    access.setHook([&](const QString &at, const QString &source, const QString &target) {
      if (phase == "before_recovery_publication" && at == "before_private_publication" && constants::isEncryptedName(path::fileName(target))) throw Checkpoint("Synthetic publication death before recovery race");
      if (at != phase) return;
      if (phase.contains("publication") && !constants::isEncryptedName(path::fileName(target))) return;
      if (injected.exchange(true)) return;
#ifndef Q_OS_WIN
      QString from = parent ? album : source;
      if (phase == "before_claimed_unlink") {
        for (const auto &entry : Directory(album).entries()) if (entry.isDir && entry.name.startsWith(".room237-vault-claim-")) from = path::join(path::join(album, entry.name), "plain");
      }
      if (!(parent ? QDir().rename(from, from + ".retired") : QFile::rename(from, from + ".retired")) || !syntheticLink(parent ? victim : outside, from, parent)) fail(QStringLiteral("Synthetic conversion exchange failed"));
#else
      if (phase == "before_source_open" && !parent) {
        if (!QFile::rename(source, source + ".retired") || !syntheticLink(outside, source)) fail(QStringLiteral("Synthetic pre-open conversion exchange failed"));
        return;
      }
      if ((parent ? QDir().rename(album, album + ".retired") : QFile::rename(source, source + ".retired"))) fail(QStringLiteral("Windows conversion did not pin the entry"));
      fail(QStringLiteral("Synthetic interruption after blocked exchange"));
#endif
    });
    ProgressWatch progress; vault_ops::encrypt(root);
    QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("incomplete"));
    QVERIFY(injected.load()); QCOMPARE(readAll(outside), outsideBytes);
    if (phase.contains("publication") || phase == "before_source_claim" || phase == "before_claimed_unlink") {
      QVERIFY(isFile(path::join(album, ".room237-vault-journal")));
      QVERIFY(!readAll(path::join(album, ".room237-vault-journal")).isEmpty());
    }
    access.setHook({}); vault::lock(root);
    bool retainedOriginal = false;
    QDirIterator files(base, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) { if (readAll(files.next()) == originalBytes) retainedOriginal = true; }
    QVERIFY(retainedOriginal);
  }

  void directoryPrivateEntriesAreRetainedOnLateExchange_data() {
    QTest::addColumn<bool>("recovery"); QTest::newRow("publication") << false; QTest::newRow("recovery") << true;
  }
  void directoryPrivateEntriesAreRetainedOnLateExchange() {
    QFETCH(bool, recovery);
    QTemporaryDir fixture; const QString base = real(fixture), root = path::join(base, "library"), victim = path::join(base, "victim");
    createDir(root); createDir(victim); write(victim, "keep", "outside");
    createDir(path::join(root, "Album")); write(path::join(root, "Album"), "original", "inside");
    bool injected = false;
    auto exchange = [&] {
      const QString service = path::join(root, ".room237-transactions");
      const auto stages = QDir(service).entryList({"album-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
      if (stages.size() != 1) fail(QStringLiteral("Missing directory claim"));
      const QString claimed = path::join(path::join(service, stages.front()), "entry");
      injected = true;
#ifndef Q_OS_WIN
      if (!QDir().rename(claimed, claimed + ".retired") || !syntheticLink(victim, claimed, true)) fail(QStringLiteral("Synthetic late directory exchange failed"));
#else
      if (QDir().rename(claimed, claimed + ".retired")) fail(QStringLiteral("Windows claim was not pinned"));
      throw Checkpoint("Synthetic interrupted blocked exchange");
#endif
    };
    {
      FileAccess access({root}); auto library = access.directory(root); auto admitted = library->childForRemoval("Album"); auto tree = album_tree::write(root);
      QVERIFY_THROWS_EXCEPTION(Error, directory_move::move(root, *library, "Album", "Renamed", admitted, [&](const QString &at) {
        if (recovery && at == "directory_claimed") throw Checkpoint("Synthetic restart");
        if (!recovery && at == "directory_before_publication") exchange();
      }));
    }
    if (recovery) QVERIFY_THROWS_EXCEPTION(Error, directory_move::recover(root, [&](const QString &at) { if (at == "directory_recovery_before_restore") exchange(); }));
    QVERIFY(injected); QCOMPARE(readAll(path::join(victim, "keep")), QByteArray("outside"));
    QVERIFY(!entryExists(path::join(root, "Renamed"))); QVERIFY(!entryExists(path::join(root, "Album")));
    bool retained = false; QDirIterator files(root, {"original"}, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) retained |= readAll(files.next()) == QByteArray("inside");
    QVERIFY(retained); QVERIFY(!listDir(path::join(root, ".room237-transactions")).empty());
  }

  void vaultPublicationRecoveryAndAlbumRelocationKeepNormalBehavior() {
    QTemporaryDir fixture; const QString root = real(fixture), originalAlbum = path::join(root, "Album"); createDir(originalAlbum);
    const QString original = image(originalAlbum, "private-name.png", qRgb(21, 43, 65)); const auto bytes = readAll(original);
    metadata::setMediaFavorite(original, true);
    vault::VaultFile file; file.id = "security-publication-recovery"; file.salt = vault::randomBytes(32); file.state = "encrypting"; file.save(root);
    vault::Keys keys; keys.id = file.id; keys.master = vault::Secret(vault::randomBytes(32)); keys.rows = vault::derive(keys.master, "rows"); keys.names = vault::derive(keys.master, "names"); vault::unlock(root, keys);
    const auto unlocked = vault::keysFor(root);
    {
      FileAccess hook(unlocked->filesystem); std::atomic<bool> interrupted = false;
      hook.setHook([&](const QString &phase, const QString &, const QString &target) {
        if (phase == "before_private_publication" && constants::isEncryptedName(path::fileName(target)) && !interrupted.exchange(true)) throw Checkpoint("Synthetic publication death");
      });
      ProgressWatch progress; vault_ops::encrypt(root); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("incomplete")); QVERIFY(interrupted.load()); hook.setHook({});
    }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    // Drop per-operation handles and resume from the sealed journal + authenticated staged ciphertext.
    { ProgressWatch progress; vault_ops::encrypt(root); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); QVERIFY2(progress.error.isEmpty(), qPrintable(progress.error)); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    album::renameAlbum(root, "Album", "Renamed");
    const QString renamed = path::join(root, "Renamed");
    const QString late = image(renamed, "late.png", qRgb(1, 9, 17)); const auto lateBytes = readAll(late);
    { ProgressWatch progress; vault_ops::encrypt(root); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    { ProgressWatch progress; vault_ops::decrypt(root); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("decrypted")); QVERIFY2(progress.error.isEmpty(), qPrintable(progress.error)); }
    QCOMPARE(readAll(path::join(renamed, "private-name.png")), bytes); QCOMPARE(readAll(late), lateBytes);
    QVERIFY(metadata::readFileEntry(path::join(renamed, "private-name.png"))->favorite); QVERIFY(!exists(path::join(root, vault::kVaultFile)));
  }

  void deletionClaimsPreserveVictimsAtEveryDestructiveBoundary_data() {
    QTest::addColumn<QString>("phase");
    for (const char *phase : {"before_delete_claim", "before_staged_claim", "before_staged_unlink", "before_delete_rollback"}) QTest::newRow(phase) << QString::fromLatin1(phase);
  }
  void deletionClaimsPreserveVictimsAtEveryDestructiveBoundary() {
    QFETCH(QString, phase); QTemporaryDir fixture;
    const QString base = real(fixture), album = path::join(base, "album"), victim = path::join(base, "victim"); createDir(album); createDir(victim);
    const QString original = image(album, "a.png", qRgb(1, 2, 3)), outside = image(victim, "a.png", qRgb(4, 5, 6)); image(album, "keep.png", qRgb(7, 8, 9));
    const auto bytes = readAll(original), outsideBytes = readAll(outside); bool injected = false;
    auto checkpoint = [&](const QString &at) {
      if (phase == "before_delete_rollback" && at == "before_commit") throw Checkpoint("Synthetic rollback");
      if (at != phase || injected) return; injected = true;
      QString from = original;
      if (phase != "before_delete_claim") {
        const auto stages = QDir(album).entryList({".room237-delete-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        if (stages.size() != 1) fail(QStringLiteral("Missing delete staging"));
        const QString stage = path::join(album, stages.front());
        if (phase == "before_staged_claim") from = path::join(stage, "a.png");
        else from = path::join(path::join(stage, "claimed-" + sha256Hex(QByteArray("a.png"))), "entry");
      }
#ifndef Q_OS_WIN
      if (!QFile::rename(from, from + ".retired") || !syntheticLink(outside, from)) fail(QStringLiteral("Synthetic deletion exchange failed"));
#else
      if (phase == "before_delete_claim") {
        if (!QFile::rename(from, from + ".retired") || !syntheticLink(outside, from)) fail(QStringLiteral("Synthetic initial deletion exchange failed"));
      } else {
        if (QFile::rename(from, from + ".retired")) fail(QStringLiteral("Windows deletion handle was not pinned"));
        throw Checkpoint("Synthetic blocked deletion exchange");
      }
#endif
    };
    try { media_delete::deleteBatchWithMode(album, {{"a.png", std::nullopt}}, {}, true, checkpoint); } catch (const Error &) {}
    QVERIFY(injected); QCOMPARE(readAll(outside), outsideBytes);
    bool retained = false; QDirIterator files(album, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) retained |= readAll(files.next()) == bytes;
    QVERIFY(retained);
    try { media_delete::recoverAlbum(album); } catch (const Error &) {}
    QCOMPARE(readAll(outside), outsideBytes); QVERIFY(isFile(path::join(album, "keep.png")));
  }

  void atomicPrivatePublicationPreservesLateReplacement() {
    QTemporaryDir fixture; const QString base = real(fixture), victim = write(base, "victim", "outside"); Directory directory(base);
#ifndef Q_OS_WIN
    QVERIFY_THROWS_EXCEPTION(Error, directory.writeAtomic("new-target", "new bytes", {}, [&](const QString &temporary) {
      const auto entry = path::join(base, temporary);
      QVERIFY(QFile::rename(entry, entry + ".retired")); QVERIFY(syntheticLink(victim, entry));
    }));
    QVERIFY(!entryExists(path::join(base, "new-target")));
    bool original = false, replacement = false;
    for (const auto &entry : directory.entries()) if (entry.name.startsWith(".room237-write-claim-")) {
      auto stage = directory.child(entry.name); original |= stage.readOptionalRegular("entry.retired") == std::optional<QByteArray>("new bytes"); replacement |= stage.info("entry").isSymlink;
    }
    QVERIFY(original && replacement);
#else
    directory.writeAtomic("new-target", "new bytes", {}, [&](const QString &temporary) { QVERIFY(!QFile::rename(path::join(base, temporary), path::join(base, "retired"))); });
    QCOMPARE(readAll(path::join(base, "new-target")), QByteArray("new bytes"));
#endif
    QCOMPARE(readAll(victim), QByteArray("outside"));
  }

  void decryptionClaimsPreserveVictimsAcrossRaces_data() {
    QTest::addColumn<QString>("phase");
    for (const char *phase : {"before_source_open", "before_private_publication", "before_recovery_publication", "before_source_claim", "before_claimed_unlink"}) QTest::newRow(phase) << QString::fromLatin1(phase);
  }
  void decryptionClaimsPreserveVictimsAcrossRaces() {
    QFETCH(QString, phase); QTemporaryDir fixture;
    const QString base = real(fixture), root = path::join(base, "library"), album = path::join(root, "Album"), victim = path::join(base, "victim"); createDir(root); createDir(album); createDir(victim);
    image(album, "photo.png", qRgb(1, 2, 3)); const QString outside = image(victim, "photo.png", qRgb(4, 5, 6)); const auto outsideBytes = readAll(outside);
    vault::VaultFile file; file.id = "security-decryption-race"; file.salt = vault::randomBytes(32); file.state = "encrypting"; file.save(root);
    vault::Keys keys; keys.id = file.id; keys.master = vault::Secret(vault::randomBytes(32)); keys.rows = vault::derive(keys.master, "rows"); keys.names = vault::derive(keys.master, "names"); vault::unlock(root, keys);
    { ProgressWatch progress; vault_ops::encrypt(root); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
    QString sealed; for (const auto &name : listDir(album)) if (constants::isEncryptedName(name)) sealed = path::join(album, name);
    QVERIFY(!sealed.isEmpty()); const auto sealedBytes = readAll(sealed);
    FileAccess access(vault::keysFor(root)->filesystem); std::atomic<bool> injected = false;
    access.setHook([&](const QString &at, const QString &source, const QString &target) {
      if (phase == "before_recovery_publication" && at == "before_private_publication" && path::fileName(target) == "photo.png") throw Checkpoint("Synthetic decryption publication death before recovery race");
      if (at != phase || (phase.contains("publication") && path::fileName(target) != "photo.png") || injected.exchange(true)) return;
      QString from = source;
      if (phase == "before_claimed_unlink") {
        const auto stages = QDir(album).entryList({".room237-vault-claim-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        if (stages.size() != 1) fail(QStringLiteral("Missing decryption claim")); from = path::join(path::join(album, stages.front()), "sealed");
      }
#ifndef Q_OS_WIN
      if (!QFile::rename(from, from + ".retired") || !syntheticLink(outside, from)) fail(QStringLiteral("Synthetic decryption exchange failed"));
#else
      if (phase == "before_source_open") {
        if (!QFile::rename(from, from + ".retired") || !syntheticLink(outside, from)) fail(QStringLiteral("Synthetic pre-open decryption exchange failed")); return;
      }
      if (QFile::rename(from, from + ".retired")) fail(QStringLiteral("Windows decryption reader was not pinned")); throw Checkpoint("Synthetic blocked exchange");
#endif
    });
    { ProgressWatch progress; vault_ops::decrypt(root); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("incomplete")); }
    QVERIFY(injected.load()); access.setHook({}); QCOMPARE(readAll(outside), outsideBytes); QVERIFY(exists(path::join(root, vault::kVaultFile))); QVERIFY(vault::keysFor(root));
    vault::lock(root); bool retained = false; QDirIterator files(album, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) retained |= readAll(files.next()) == sealedBytes; QVERIFY(retained);
  }

  void movePreparatoryRecoveryRefusesExchangedLocator() {
    QTemporaryDir fixture; const QString base = real(fixture), source = path::join(base, "source"), target = path::join(base, "target"), victim = path::join(base, "victim"); createDir(source); createDir(target); createDir(victim);
    const QString original = image(source, "a.png", qRgb(1, 2, 3)); const auto originalBytes = readAll(original); const QString sentinel = write(victim, "keep", "outside"); QString staging;
    { auto journal = media_move::MoveJournal::create(source, target); media_move::FileRecord record{"a.png", "a.png", *metadata::sourceRevision(original)}; journal->recordFiles({record}); journal->moveFile(record, [](const QString &) {}); staging = journal->staging(); }
    bool injected = false;
    QVERIFY_THROWS_EXCEPTION(Error, media_move::recoverAlbumWithHook(source, [&](const QString &at) {
      if (at != "move_recovery_header_read" || injected) return; injected = true;
#ifndef Q_OS_WIN
      if (!QDir().rename(staging, staging + ".retired") || !syntheticLink(victim, staging, true)) fail(QStringLiteral("Synthetic locator exchange failed"));
#else
      if (QDir().rename(staging, staging + ".retired")) fail(QStringLiteral("Windows recovery locator was not pinned")); throw Checkpoint("Synthetic blocked exchange");
#endif
    }));
    QVERIFY(injected); QCOMPARE(readAll(sentinel), QByteArray("outside")); QCOMPARE(readAll(path::join(target, "a.png")), originalBytes); QVERIFY(!entryExists(original));
  }

  void admittedDecoderBridgeRemainsSeekableForRealFfmpeg() {
    QTemporaryDir fixture; const QString base = real(fixture), source = image(base, "photo.png", qRgb(12, 34, 56)); FileAccess access({base}); access.pin(source);
    const auto result = runProcess(ffmpegPath(), {"-v", "error", "-i", decoderPath(source), "-f", "null", "-"}, 10000);
    QVERIFY2(result.success, result.stderrBytes.constData());
  }

  void largeMediaMovesKeepBoundedHandlesAndMetadata() {
    QTemporaryDir fixture; const QString base = real(fixture), source = path::join(base, "source"), target = path::join(base, "target"); createDir(source); createDir(target);
    QStringList names; for (int i = 0; i < 180; ++i) { const QString name = QString::number(i) + ".png"; names << name; image(source, name, qRgb(i, 2, 3)); }
    metadata::setMediaFavorite(path::join(source, names.front()), true);
#ifndef Q_OS_WIN
    auto descriptors = [] { int n = 0; for (int fd = 0; fd < 4096; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++n; return n; };
    const int baseline = descriptors(); int high = baseline;
#endif
    const auto failed = album::moveMediaBatchWithHook(source, target, names, true, [&](const QString &, const QString &, const QString &) {
#ifndef Q_OS_WIN
      high = std::max(high, descriptors());
#endif
    });
    QVERIFY(failed.empty());
#ifndef Q_OS_WIN
    QVERIFY2(high < baseline + 48, qPrintable(QString("descriptor growth %1").arg(high - baseline)));
#endif
    for (const auto &name : names) { QVERIFY(isFile(path::join(target, name))); QVERIFY(!entryExists(path::join(source, name))); }
    QVERIFY(metadata::readFileEntry(path::join(target, names.front()))->favorite); QVERIFY(!media_move::hasJournal(source));
  }

  void writableReopenChecksIdentityBeforeTruncation() {
    QTemporaryDir fixture; const QString base = real(fixture), source = write(base, "source", "original"), victim = write(base, "victim", "outside");
    FileAccess access({base}); access.pin(source);
#ifndef Q_OS_WIN
    QVERIFY(QFile::rename(source, source + ".retired"));
    QVERIFY(::link(QFile::encodeName(victim).constData(), QFile::encodeName(source).constData()) == 0);
    QVERIFY_THROWS_EXCEPTION(Error, access.open(source, QIODevice::WriteOnly));
#else
    QVERIFY(!QFile::rename(source, source + ".retired"));
#endif
    QCOMPARE(readAll(victim), QByteArray("outside"));
    QCOMPARE(readAll(source +
#ifndef Q_OS_WIN
      QStringLiteral(".retired")
#else
      QString()
#endif
    ), QByteArray("original"));
  }

  void conversionRecoveryResumesAfterEachClaimPhase_data() {
    QTest::addColumn<bool>("encrypt"); QTest::addColumn<QString>("phase");
    for (bool encrypt : {true, false}) for (const char *phase : {"before_private_publication", "before_source_claim", "before_claimed_unlink", "before_encrypt_write", "after_encrypt_write", "after_encrypt_metadata", "after_decrypt_source_removal"})
      if ((encrypt && !QString::fromLatin1(phase).startsWith("after_decrypt")) || (!encrypt && !QString::fromLatin1(phase).contains("encrypt")))
        QTest::newRow(qPrintable(QString(encrypt ? "encrypt-" : "decrypt-") + QString::fromLatin1(phase))) << encrypt << QString::fromLatin1(phase);
  }
  void conversionRecoveryResumesAfterEachClaimPhase() {
    QFETCH(bool, encrypt); QFETCH(QString, phase); QTemporaryDir fixture; const QString root = real(fixture), original = image(root, "original.png", qRgb(5, 9, 13)); const auto bytes = readAll(original);
    vault::VaultFile file; file.id = "security-phase-recovery"; file.salt = vault::randomBytes(32); file.state = "encrypting"; file.save(root);
    vault::Keys keys; keys.id = file.id; keys.master = vault::Secret(vault::randomBytes(32)); keys.rows = vault::derive(keys.master, "rows"); keys.names = vault::derive(keys.master, "names"); vault::unlock(root, keys);
    if (!encrypt) { ProgressWatch progress; vault_ops::encrypt(root); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    FileAccess access(vault::keysFor(root)->filesystem); std::atomic<bool> interrupted = false;
    access.setHook([&](const QString &at, const QString &, const QString &target) {
      if ((at != phase && !(phase == "before_private_publication" && at == "before_recovery_publication")) ||
          (phase.contains("publication") && (encrypt ? !constants::isEncryptedName(path::fileName(target)) : path::fileName(target) != "original.png"))) return;
      interrupted = true; throw Checkpoint("Synthetic conversion phase death");
    });
    { ProgressWatch progress; if (encrypt) vault_ops::encrypt(root); else vault_ops::decrypt(root); QCOMPARE(progress.wait({"encrypted", "decrypted", "incomplete"}), QString("incomplete")); }
    QVERIFY(interrupted.load()); QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000); access.setHook({});
    if (phase == "before_private_publication" || phase == "before_source_claim" || phase == "before_claimed_unlink" || phase == "after_encrypt_metadata")
      QVERIFY(isFile(path::join(root, ".room237-vault-journal")));
    { ProgressWatch progress; if (encrypt) vault_ops::encrypt(root); else vault_ops::decrypt(root); QCOMPARE(progress.wait({"encrypted", "decrypted", "incomplete"}), QString(encrypt ? "encrypted" : "decrypted")); QVERIFY2(progress.error.isEmpty(), qPrintable(progress.error)); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    if (encrypt) { ProgressWatch progress; vault_ops::decrypt(root); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("decrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QCOMPARE(readAll(original), bytes); QVERIFY(!exists(path::join(root, vault::kVaultFile)));
  }

  void absoluteDirectoryAdmissionUsesBoundedDescriptors() {
#ifndef Q_OS_WIN
    QTemporaryDir fixture;
    auto count = [] { int n = 0; for (int fd = 0; fd < 1024; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++n; return n; };
    const int before = count();
    std::vector<Directory> directories;
    for (int i = 0; i < 16; ++i) directories.emplace_back(real(fixture));
    QVERIFY(count() <= before + 32);
#else
    QSKIP("POSIX descriptor budget; Windows pins pathname ancestors with handles");
#endif
  }

  void albumDeletionKeepsNormalNestedBehavior() {
    QTemporaryDir fixture;
    const QString root = real(fixture), album = path::join(root, "Album");
    createDir(album); createDir(path::join(album, "Nested"));
    write(path::join(album, "Nested"), "keep-until-delete.txt", "original");
    album::deleteAlbum(root, "Album");
    QVERIFY(!entryExists(album));
  }

  void vaultCatalogDoesNotOpenDiskTemporaries() {
    QTemporaryDir fixture;
    const QString base = real(fixture), root = path::join(base, "vault"), plain = path::join(base, "plain");
    createDir(root); createDir(plain);
    vault::VaultFile file; file.id = "security-catalog-memory"; file.salt = vault::randomBytes(32); file.state = "encrypted";
    file.save(root);
    vault::Keys keys; keys.id = file.id; keys.master = vault::Secret(vault::randomBytes(32));
    keys.rows = vault::derive(keys.master, "rows"); keys.names = vault::derive(keys.master, "names");
    vault::unlock(root, keys);
    QStringList ids;
    for (int i = 0; i < 50000; ++i) ids << QString::number(i) + QString(200, QLatin1Char('x'));
    {
      AuditScope audit;
      catalog::initialize(root);
      catalog::retainAlbums(root, ids);
      QCOMPARE(TempIoAudit::instance().temporary.load(), 0);
      // Positive control: the ordinary catalog keeps its FILE policy and this fixture really exceeds the temp cache.
      catalog::initialize(plain);
      catalog::retainAlbums(plain, ids);
      QVERIFY(TempIoAudit::instance().temporary.load() > 0);
      catalog::releaseMemory(root);
    }
    vault::lock(root);
  }

  void processOutputIsDrainedAndBinaryIsNotDiagnosticTruncated() {
    const QString executable = QCoreApplication::applicationFilePath();
    const auto result = runProcess(executable, {"--security-output-helper"}, 30000);
    QVERIFY(result.success);
    QCOMPARE(result.stdoutBytes, QByteArray(4 << 20, 'O'));
    QCOMPARE(result.stderrBytes, QByteArray(4 << 20, 'E'));
    QBuffer input; input.setData("synthetic stdin"); input.open(QIODevice::ReadOnly);
    const auto binary = runWithInput(executable, {"--security-png-helper"}, input, 30000);
    QVERIFY(binary.success);
    QVERIFY(binary.stdoutBytes.size() > (4 << 20));
    QVERIFY(!QImage::fromData(binary.stdoutBytes, "PNG").isNull());
    QCOMPARE(binary.stderrBytes, QByteArray(4 << 20, 'E'));
    QVERIFY_THROWS_EXCEPTION(Error, runBinaryProcess(executable, {"--security-output-helper"}, 30000, 1024));
    QVERIFY_THROWS_EXCEPTION(Error, runProcess(executable, {"--security-idle-helper"}, 20));
    const auto probe = runProcess(executable, {"--security-memory-probe"}, 60000);
    QVERIFY2(probe.success, probe.stderrBytes.constData());
    qInfo("bounded diagnostic buffering: %s", probe.stderrBytes.constData());
  }

  // --- Formats shared with earlier releases ---------------------------------------------------------------------

  void packedMetaRoundTrips() {
    DetachedFileMeta meta{quint64(1700000000), qint64(-86400 * 365 * 40), true, false, quint32(4032), quint32(3024)};
    const DetachedFileMeta back = unpackFileMeta(meta.pack());
    QCOMPARE(back.a, meta.a);
    QCOMPARE(back.s, meta.s);
    QCOMPARE(back.w, meta.w);
    QCOMPARE(back.h, meta.h);
    QVERIFY(back.i && !back.v);
    // Absent fields stay absent, and a value without its presence bit is not reported.
    const DetachedFileMeta empty = unpackFileMeta(DetachedFileMeta{}.pack());
    QVERIFY(!empty.a && !empty.s && !empty.w && !empty.h);
    QCOMPARE(DetachedFileMeta{}.pack(), QStringLiteral("0"));
    QVERIFY_THROWS_EXCEPTION(Error, unpackFileMeta(QStringLiteral("12x")));
    QVERIFY(!validPackedMeta(QStringLiteral("340282366920938463463374607431768211456")));  // 2^128
  }

  void fileMetaEntryUsesTheStoredFieldOrder() {
    FileMetaEntry entry;
    entry.blurPreview = QStringLiteral("b");
    entry.meta = QStringLiteral("1");
    entry.favorite = true;
    entry.hashBits = 256;
    entry.thumbVersion = QStringLiteral("v");
    QCOMPARE(entry.toJson(), QStringLiteral(R"({"blur_preview":"b","meta":"1","favorite":true,"hash":null,"hash_version":null,"hash_bits":256,"thumb_version":"v","hash_failed":false,"thumb_failed":false,"meta_failed":false})"));
    QCOMPARE(FileMetaEntry::fromJson(entry.toJson()), entry);
    // A record of an earlier release with unknown and missing fields still loads.
    const FileMetaEntry legacy = FileMetaEntry::fromJson(QStringLiteral(R"({"meta":"5","favorite":true,"unknown":1})"));
    QCOMPARE(legacy.meta, std::optional<QString>(QStringLiteral("5")));
    QVERIFY(legacy.favorite);
    QVERIFY_THROWS_EXCEPTION(Error, FileMetaEntry::fromJson(QStringLiteral(R"({"favorite":"yes"})")));
  }

  void jsonNumbersAndStringsMatchSerde() {
    QCOMPARE(json::float32(1.0f), QStringLiteral("1.0"));
    QCOMPARE(json::float32(2.5f), QStringLiteral("2.5"));
    QCOMPARE(json::float32(0.1f), QStringLiteral("0.1"));
    QCOMPARE(json::string(QStringLiteral("a\"b\\c\n\x01")), QStringLiteral("\"a\\\"b\\\\c\\n\\u0001\""));
  }

  void settingsVersionsKeepTheirEarlierValues() {
    const AdvancedSettings settings = AdvancedSettings().clamp();
    QCOMPARE(settings.thumbVersionFor(QStringLiteral("ffmpeg")), QStringLiteral("11f1dc9bc9064a7b"));
    QCOMPARE(settings.thumbVersionFor(QStringLiteral("imageio-libwebp-1")), QStringLiteral("a8e5ef8b47c6545b"));
    QCOMPARE(settings.metadataVersion(), QStringLiteral("signed-date-v2|display-size-v1|creation-time:true"));
#ifdef Q_OS_MACOS
    // Recorded by the earlier core for the default settings on macOS.
    QCOMPARE(settings.hashConfig().hashVersion, QStringLiteral("006f31998b41b5ccdbdf98858c4d74e3389da245290b739173bd5b2267fb81fb"));
#endif
    const QString encoded = QStringLiteral(R"([{"maxDim":450,"imageWebpQuality":75,"imageWebpCompressionLevel":2,"videoSeekSeconds":1.0,"lockPollMs":50},{"threads":4,"timeoutSecs":5,"hwaccel":"auto","processWaitPollMs":50},{"ffmpegProbeTimeoutSecs":5,"parseCreationTime":true},"%1"])").arg(settings.hashConfig().hashVersion);
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(QByteArrayLiteral("blur-webp-16-v1|"));
    hasher.addData(encoded.toUtf8());
    QCOMPARE(settings.processingVersion(), QString::fromLatin1(hasher.result().toHex()).left(16));
  }

  void corruptSettingsAreRefusedNotReplaced() {
    QTemporaryDir dir;
    const QString file = write(dir.path(), QStringLiteral("settings.json"), "{broken");
    QVERIFY_THROWS_EXCEPTION(Error, loadSettingsFile(file));
    QCOMPARE(readAll(file), QByteArray("{broken"));
    write(dir.path(), QStringLiteral("settings.json"), R"({"privacy":{"enabled":true}})");
    QVERIFY_THROWS_EXCEPTION(Error, loadSettingsFile(file));  // A present section must be complete.
    write(dir.path(), QStringLiteral("settings.json"), R"({"privacy":{"enabled":true,"lockscreenEnabled":false,"confirmOpenEnabled":false}})");
    const AdvancedSettings loaded = loadSettingsFile(file);
    QVERIFY(loaded.privacy.lockscreenEnabled && loaded.privacy.confirmOpenEnabled);  // Privacy implies both.
  }

  // --- Album database -------------------------------------------------------------------------------------------

  void legacyJsonMigratesOnce() {
    QTemporaryDir root;
    const QString album = real(root);
    createDir(path::join(album, QStringLiteral(".room237-metadata")));
    write(path::join(album, QStringLiteral(".room237-metadata")), QStringLiteral("album.json"), R"({"files":{"a.jpg":{"meta":"7","favorite":true}},"duplicates_ignore":[["a.jpg","b.jpg"]]})");
    metadata::prepareDatabase(album);
    const AlbumMeta meta = metadata::readAlbumMeta(album);
    QVERIFY(meta.files.value(QStringLiteral("a.jpg")).favorite);
    QCOMPARE(meta.duplicatesIgnore.size(), size_t(1));
    // The legacy file is kept as a backup and no longer read.
    QVERIFY(isFile(path::join(album, QStringLiteral(".room237-metadata/album.json"))));
  }

  void corruptLegacyMetadataSurvives() {
    QTemporaryDir root;
    const QString album = real(root);
    createDir(path::join(album, QStringLiteral(".room237-metadata")));
    write(path::join(album, QStringLiteral(".room237-metadata")), QStringLiteral("album.json"), "{not json");
    QVERIFY_THROWS_EXCEPTION(Error, metadata::prepareDatabase(album));
    QVERIFY(!exists(path::join(album, QStringLiteral(".room237-metadata/album.sqlite"))));
    QCOMPARE(readAll(path::join(album, QStringLiteral(".room237-metadata/album.json"))), QByteArray("{not json"));
  }

  void versionOneDatabaseUpgrades() {
    QTemporaryDir root;
    const QString album = real(root);
    createDir(path::join(album, QStringLiteral(".room237-metadata")));
    {
      db::Connection conn(path::join(album, QStringLiteral(".room237-metadata/album.sqlite")));
      conn.executeBatch(QStringLiteral("CREATE TABLE files(name TEXT PRIMARY KEY,data TEXT NOT NULL); CREATE TABLE album(id INTEGER PRIMARY KEY,ignores TEXT NOT NULL);"
                                       "INSERT INTO album VALUES(1,'[[\"a.jpg\",\"b.jpg\"]]'); PRAGMA user_version=1;"));
    }
    const AlbumMeta meta = metadata::readAlbumMeta(album);
    QCOMPARE(meta.duplicatesIgnore.size(), size_t(1));
    db::Connection conn(path::join(album, QStringLiteral(".room237-metadata/album.sqlite")));
    QCOMPARE(conn.pragmaInteger(QStringLiteral("user_version")), qint64(2));
  }

  void derivedWriteRefusesChangedMedia() {
    QTemporaryDir root;
    const QString album = real(root);
    const QString file = image(album, QStringLiteral("a.png"), 0xff0000);
    const QString source = *metadata::sourceRevision(file);
    write(album, QStringLiteral("a.png"), "changed");
    QVERIFY_THROWS_EXCEPTION(Error, metadata::updateFileMetaIfSourceCurrent(file, source, [](FileMetaEntry &entry) { entry.favorite = true; }));
    QVERIFY(!metadata::readFileEntry(file).value_or(FileMetaEntry()).favorite);
  }

  void favoriteAndMetadataPersist() {
    QTemporaryDir root;
    const QString album = real(root);
    const QString file = image(album, QStringLiteral("a.png"), 0x00ff00);
    const DetachedMediaEntry entry = metadata::setMediaFavorite(file, true);
    QCOMPARE(entry.favorite, std::optional<bool>(true));
    const DetachedFileMeta meta = unpackFileMeta(entry.meta);
    QCOMPARE(meta.w, std::optional<quint32>(64));
    QCOMPARE(meta.h, std::optional<quint32>(48));
    QVERIFY(meta.i);
    QVERIFY(metadata::readFileEntry(file)->favorite);
  }

#ifndef Q_OS_WIN
  void manualDateLivesInTheFileAndTheDatabase() {
    QTemporaryDir root;
    const QString album = real(root);
    image(album, QStringLiteral("a.png"), 0x0000ff);
    const auto updated = metadata::setMediaTimestamp(album, {QStringLiteral("a.png")}, -1000);
    QCOMPARE(updated.size(), size_t(1));
    QCOMPARE(unpackFileMeta(updated[0].meta).s, std::optional<qint64>(-1000));
    QCOMPARE(metadata::readEmbeddedTimestamp(path::join(album, QStringLiteral("a.png"))), std::optional<qint64>(-1000));
    QVERIFY_THROWS_EXCEPTION(Error, metadata::setMediaTimestamp(album, {QStringLiteral("a.png")}, qint64(1) << 41));
  }
#endif

  // --- Journaled deletion ---------------------------------------------------------------------------------------

  void deletionRemovesFileMetadataAndPreview() {
    QTemporaryDir root;
    const QString album = real(root);
    const QString file = image(album, QStringLiteral("a.png"), 0x123456);
    metadata::setMediaFavorite(file, true);
    createDerivedDir(path::join(album, QStringLiteral(".room237-thumb")));
    write(path::join(album, QStringLiteral(".room237-thumb")), QStringLiteral("a.png.webp"), "preview");
    media_delete::deleteMedia(file);
    QVERIFY(!entryExists(file));
    QVERIFY(!entryExists(path::join(album, QStringLiteral(".room237-thumb/a.png.webp"))));
    QVERIFY(!metadata::readFileEntry(file));
    for (const DirEntry &entry : readDir(album)) QVERIFY2(!entry.name.startsWith(QLatin1String(".room237-delete-")), qPrintable(entry.name));
  }

  void deletionInterruptedBeforeCommitRestoresOriginals() {
    QTemporaryDir root;
    const QString album = real(root);
    image(album, QStringLiteral("a.png"), 0x111111);
    image(album, QStringLiteral("b.png"), 0x222222);
    const QByteArray before = readAll(path::join(album, QStringLiteral("a.png")));
    QVERIFY_THROWS_EXCEPTION(Error, media_delete::deleteBatchWithMode(album, {{QStringLiteral("a.png"), std::nullopt}, {QStringLiteral("b.png"), std::nullopt}}, {}, true, [](const QString &phase) {
      if (phase == QLatin1String("before_commit")) throw Checkpoint(QStringLiteral("simulated crash"));
    }));
    QCOMPARE(readAll(path::join(album, QStringLiteral("a.png"))), before);
    QVERIFY(isFile(path::join(album, QStringLiteral("b.png"))));
  }

  void deletionCommittedButUnfinishedCompletesOnRecovery() {
    QTemporaryDir root;
    const QString album = real(root);
    image(album, QStringLiteral("a.png"), 0x333333);
    image(album, QStringLiteral("keep.png"), 0x444444);
    try {
      media_delete::deleteBatchWithMode(album, {{QStringLiteral("a.png"), std::nullopt}}, {}, true, [](const QString &phase) {
        if (phase == QLatin1String("committed")) throw Checkpoint(QStringLiteral("simulated crash"));
      });
      QFAIL("the checkpoint should have interrupted the deletion");
    } catch (const Checkpoint &) {
    }
    QVERIFY(!entryExists(path::join(album, QStringLiteral("a.png"))));
    bool staged = false;
    for (const DirEntry &entry : readDir(album)) staged |= entry.name.startsWith(QLatin1String(".room237-delete-"));
    QVERIFY(staged);
    media_delete::recoverAlbum(album);
    for (const DirEntry &entry : readDir(album)) QVERIFY2(!entry.name.startsWith(QLatin1String(".room237-delete-")), qPrintable(entry.name));
    QVERIFY(isFile(path::join(album, QStringLiteral("keep.png"))));
  }

  void guardedDeletionNeedsCurrentRevisions() {
    QTemporaryDir root;
    const QString album = real(root);
    const QString a = image(album, QStringLiteral("a.png"), 0x555555);
    const QString b = image(album, QStringLiteral("b.png"), 0x666666);
    const QString revision = metadata::mediaRevision(a);
    const std::vector<metadata::ObservedMedia> observed{{b, metadata::mediaRevision(b)}};
    write(album, QStringLiteral("a.png"), "edited after review");
    QVERIFY_THROWS_EXCEPTION(Error, media_delete::deleteMediaBatch(album, {{QStringLiteral("a.png"), revision}}, observed));
    QVERIFY(isFile(a));
    QVERIFY_THROWS_EXCEPTION(Error, media_delete::deleteMediaBatch(album, {{QStringLiteral("b.png"), metadata::mediaRevision(b)}}, observed));  // Must retain one.
  }

  // --- Journaled moves ------------------------------------------------------------------------------------------

  void moveCarriesMetadataAndPreview() {
    QTemporaryDir root;
    const QString base = real(root);
    const QString source = path::join(base, QStringLiteral("src")), target = path::join(base, QStringLiteral("dst"));
    createDir(source);
    createDir(target);
    const QString file = image(source, QStringLiteral("a.png"), 0x777777);
    metadata::setMediaFavorite(file, true);
    createDerivedDir(path::join(source, QStringLiteral(".room237-thumb")));
    write(path::join(source, QStringLiteral(".room237-thumb")), QStringLiteral("a.png.webp"), "preview");
    image(target, QStringLiteral("a.png"), 0x888888);  // A name collision gets a suffix.
    const QStringList failed = album::moveMediaBatchWithHook(source, target, {QStringLiteral("a.png")}, true, [](const QString &, const QString &, const QString &) {});
    QVERIFY(failed.isEmpty());
    QVERIFY(!entryExists(file));
    QVERIFY(isFile(path::join(target, QStringLiteral("a_1.png"))));
    QVERIFY(metadata::readFileEntry(path::join(target, QStringLiteral("a_1.png")))->favorite);
    QCOMPARE(readAll(path::join(target, QStringLiteral(".room237-thumb/a_1.png.webp"))), QByteArray("preview"));
    QVERIFY(!media_move::hasJournal(source));
  }

  void moveInterruptedBeforeMetadataRollsBack() {
    QTemporaryDir root;
    const QString base = real(root);
    const QString source = path::join(base, QStringLiteral("src")), target = path::join(base, QStringLiteral("dst"));
    createDir(source);
    createDir(target);
    const QString file = image(source, QStringLiteral("a.png"), 0x999999);
    metadata::setMediaFavorite(file, true);
    QVERIFY_THROWS_EXCEPTION(Error, album::moveMediaBatchWithHook(source, target, {QStringLiteral("a.png")}, true, [](const QString &phase, const QString &, const QString &) {
      if (phase == QLatin1String("after_target_metadata")) throw Checkpoint(QStringLiteral("simulated failure"));
    }));
    QVERIFY(isFile(file));
    QVERIFY(!entryExists(path::join(target, QStringLiteral("a.png"))));
    QVERIFY(metadata::readFileEntry(file)->favorite);
    QVERIFY(!media_move::hasJournal(source));
  }

  void moveCrashBeforeCommitRecoversOnNextAccess() {
    QTemporaryDir root;
    const QString base = real(root);
    const QString source = path::join(base, QStringLiteral("src")), target = path::join(base, QStringLiteral("dst"));
    createDir(source);
    createDir(target);
    const QString file = image(source, QStringLiteral("a.png"), 0xaaaaaa);
    // A crash leaves the journal behind: emulate it by abandoning the operation without its rollback.
    {
      auto journal = media_move::MoveJournal::create(source, target);
      journal->recordFiles({{QStringLiteral("a.png"), QStringLiteral("a.png"), *metadata::sourceRevision(file)}});
      renameReplace(file, path::join(target, QStringLiteral("a.png")));
    }
    QVERIFY(media_move::hasJournal(source));
    media_move::recoverAlbum(source);
    QVERIFY(isFile(file));
    QVERIFY(!entryExists(path::join(target, QStringLiteral("a.png"))));
    QVERIFY(!media_move::hasJournal(source));
    for (const DirEntry &entry : readDir(target)) QVERIFY2(!entry.name.startsWith(QLatin1String(".room237-move-")), qPrintable(entry.name));
  }

  // --- Albums ---------------------------------------------------------------------------------------------------

  void albumRenameAndDeleteStayInsideTheRoot() {
    QTemporaryDir root;
    const QString base = real(root);
    createDir(path::join(base, QStringLiteral("a")));
    const album::RenamedAlbumResult renamed = album::renameAlbum(base, QStringLiteral("a"), QStringLiteral("b/c"));
    QCOMPARE(renamed.name, QStringLiteral("b_c"));
    QVERIFY(isDir(path::join(base, QStringLiteral("b_c"))));
    QVERIFY_THROWS_EXCEPTION(Error, album::deleteAlbum(base, QStringLiteral("../x")));
    QVERIFY_THROWS_EXCEPTION(Error, album::deleteAlbum(base, QStringLiteral(".room237-catalog")));
    album::deleteAlbum(base, QStringLiteral("b_c"));
    QVERIFY(!exists(path::join(base, QStringLiteral("b_c"))));
  }

  void catalogPagesFavoritesAndNeighbors() {
    QTemporaryDir root;
    const QString base = real(root);
    const QString album = path::join(base, QStringLiteral("a"));
    createDir(album);
    for (int i = 0; i < 5; ++i) image(album, QStringLiteral("%1.png").arg(i), qRgb(i * 40, 10, 10));
    catalog::initialize(base);
    QCOMPARE(catalog::scanAlbum(base, album), 5);
    metadata::setMediaFavorite(path::join(album, QStringLiteral("3.png")), true);
    catalog::PageQuery query;
    query.rootDir = base;
    query.albumId = QStringLiteral("a");
    query.sortKey = QStringLiteral("name");
    query.sortDir = QStringLiteral("asc");
    QCOMPARE(catalog::queryPage(query).total, quint64(5));
    QCOMPARE(catalog::queryPage(query).items.front().media.name, QStringLiteral("0.png"));
    const auto next = catalog::getMediaNeighbor(query, path::join(album, QStringLiteral("4.png")), 1);
    QCOMPARE(next->media.name, QStringLiteral("0.png"));  // Wraps around.
    query.favoritesOnly = true;
    QCOMPARE(catalog::queryPage(query).total, quint64(1));
  }

  // --- Updates --------------------------------------------------------------------------------------------------

  void signedManifestVerifies() {
    const QString key = QString::fromUtf8(readAll(QStringLiteral(ROOM237_TESTDATA "/update/key.pub"))).trimmed();
    const QByteArray manifest = readAll(QStringLiteral(ROOM237_TESTDATA "/update/manifest.json"));
    const QString signature = QString::fromUtf8(readAll(QStringLiteral(ROOM237_TESTDATA "/update/manifest.json.minisig")));
    update::verifySignature(key, manifest, signature);
    QByteArray tampered = manifest;
    tampered[10] = tampered[10] == '1' ? '2' : '1';
    QVERIFY_THROWS_EXCEPTION(Error, update::verifySignature(key, tampered, signature));
    QString trusted = signature;
    trusted.replace(QStringLiteral("file:manifest.json"), QStringLiteral("file:other.json"));
    QVERIFY_THROWS_EXCEPTION(Error, update::verifySignature(key, manifest, trusted));
  }

  void versionsCompareNumerically() {
    QVERIFY(update::isNewer(QStringLiteral("7.10"), QStringLiteral("7.9")));
    QVERIFY(!update::isNewer(QStringLiteral("7.0.0"), QStringLiteral("7.0")));
    QVERIFY(update::isNewer(QStringLiteral("7.1-beta"), QStringLiteral("7.0.9")));
    QVERIFY_THROWS_EXCEPTION(Error, update::isNewer(QStringLiteral("seven"), QStringLiteral("7.0")));
  }

  void unpackRefusesEscapes() {
    QTemporaryDir dir;
    const QString base = real(dir);
    const QString good = path::join(base, QStringLiteral("good.tar.gz"));
    gzipTo(good, tarEntry("Contents/", '5') + tarEntry("Contents/MacOS/app", '0', "binary") + tarEntry("Contents/link", '2', {}, "MacOS/app"));
    const QString out = path::join(base, QStringLiteral("out"));
    createDir(out);
    update::unpack(good, out);
    QCOMPARE(readAll(path::join(out, QStringLiteral("Contents/MacOS/app"))), QByteArray("binary"));
    for (const auto &[name, entry] : std::vector<std::pair<const char *, QByteArray>>{
             {"parent", tarEntry("../evil", '0', "x")},
             {"absolute", tarEntry("/tmp/evil", '0', "x")},
             {"link", tarEntry("Contents/escape", '2', {}, "../../outside")},
             {"device", tarEntry("Contents/dev", '3')}}) {
      const QString archive = path::join(base, QString::fromLatin1(name) + QStringLiteral(".tar.gz"));
      gzipTo(archive, entry);
      const QString target = path::join(base, QStringLiteral("out-") + QString::fromLatin1(name));
      createDir(target);
      QVERIFY_THROWS_EXCEPTION(Error, update::unpack(archive, target));
    }
  }


  // --- Vault --------------------------------------------------------------------------------------------------------

  void encryptedFilesRoundTripWithRandomAccess() {
    QTemporaryDir dir;
    const QString base = real(dir);
    vault::Keys keys;
    keys.master = vault::Secret(vault::randomBytes(32));
    keys.rows = vault::derive(keys.master, "rows");
    keys.names = vault::derive(keys.master, "names");
    QByteArray plain;
    for (int i = 0; i < 200000; ++i) plain.append(char(i * 7));
    const QString source = write(base, QStringLiteral("clip.mov"), plain);
    vault::FileMeta meta;
    meta.name = QStringLiteral("clip.mov");
    meta.mtimeNs = 1700000000123456789LL;
    const QString target = path::join(base, vault::newPhysicalName());
    const vault::FileMeta stored = vault::encryptFile(keys, source, target, meta);
    QCOMPARE(stored.name, QStringLiteral("clip.mov"));
    QCOMPARE(stored.size, quint64(plain.size()));
    QCOMPARE(stored.sha256, QCryptographicHash::hash(plain, QCryptographicHash::Sha256));
    QVERIFY(vault::isEncryptedFile(target));
    QVERIFY(!readAll(target).contains(plain.mid(1000, 64)));
    QCOMPARE(stat(target).mtimeNs, meta.mtimeNs);
    QCOMPARE(vault::decryptAll(keys, target), plain);
    vault::DecryptingDevice device(std::make_shared<vault::Keys>(keys), target);
    QVERIFY(device.open(QIODevice::ReadOnly));
    QVERIFY(device.seek(131070));
    QCOMPARE(device.read(10), plain.mid(131070, 10));  // Across a chunk boundary.
    // The name can change without rewriting the content, and the file keeps its time.
    vault::FileMeta renamed = stored;
    renamed.name = QStringLiteral("renamed.mov");
    vault::writeMeta(keys, target, renamed);
    QCOMPARE(vault::readMeta(keys, target).name, QStringLiteral("renamed.mov"));
    QCOMPARE(stat(target).mtimeNs, meta.mtimeNs);
    const QString restored = path::join(base, QStringLiteral("restored.mov"));
    vault::decryptToFile(keys, target, restored);
    QCOMPARE(readAll(restored), plain);
    // Another key or a flipped byte is refused.
    vault::Keys other = keys;
    other.master = vault::Secret(vault::randomBytes(32));
    QVERIFY_THROWS_EXCEPTION(Error, vault::readMeta(other, target));
    QFile damaged(target);
    QVERIFY(damaged.open(QIODevice::ReadWrite));
    damaged.seek(9000);
    const char flip = damaged.read(1)[0];
    damaged.seek(9000);
    damaged.write(QByteArray(1, char(flip ^ 1)));
    damaged.close();
    QVERIFY_THROWS_EXCEPTION(Error, vault::decryptAll(keys, target));
  }

  void argon2Timing() {
    for (const auto &[memory, passes] : {std::pair{65536u, 3u}, std::pair{131072u, 3u}, std::pair{262144u, 2u}}) {
      QElapsedTimer clock;
      clock.start();
      vault::argon2id("password", "0123456789abcdef0123456789abcdef", memory, passes);
      qInfo("argon2id %u KiB x%u: %lld ms", memory, passes, clock.elapsed());
    }
  }

  void slotsAndRecoveryKeys() {
    const vault::Secret p = vault::argon2id("correct horse", "0123456789abcdef", 8192, 1);
    const vault::Secret s(vault::randomBytes(32));
    const vault::Secret kek = vault::slotKey(p, QStringLiteral("macos-se"), s);
    const vault::Secret master(vault::randomBytes(32));
    const QByteArray wrap = vault::seal(kek, master.bytes());
    QCOMPARE(*vault::open(kek, wrap), master.bytes());
    const vault::Secret wrong = vault::argon2id("correct horsf", "0123456789abcdef", 8192, 1);
    QVERIFY(!vault::open(vault::slotKey(wrong, QStringLiteral("macos-se"), s), wrap));
    QVERIFY(!vault::open(vault::slotKey(p, QStringLiteral("fido2"), s), wrap));
    const QString recovery = vault::newRecoveryKey();
    QVERIFY(vault::parseRecoveryKey(recovery));
    QVERIFY(vault::parseRecoveryKey(recovery.toLower().remove(QLatin1Char('-'))));
    QString typo = recovery;
    typo[0] = typo[0] == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
    QVERIFY(!vault::parseRecoveryKey(typo));
  }


  // --- Security hardening -----------------------------------------------------------------------------------------

  void conversionProcessExitRecovery() {
    auto fixtureKeys = [] {
      vault::Keys keys; keys.id = "process-exit-fixture";
      keys.master = vault::Secret(QByteArray(32, 't'));
      keys.rows = vault::derive(keys.master, "rows"); keys.names = vault::derive(keys.master, "names");
      return keys;
    };
    const QString childRoot = qEnvironmentVariable("ROOM237_TEST_EXIT_ROOT");
    if (!childRoot.isEmpty()) {
      vault::unlock(childRoot, fixtureKeys());
      FileAccess access(vault::keysFor(childRoot)->filesystem);
      const QString phase = qEnvironmentVariable("ROOM237_TEST_EXIT_PHASE");
      access.setHook([phase](const QString &at, const QString &, const QString &target) {
        if (at == phase && (at != "before_private_publication" || constants::isEncryptedName(path::fileName(target)))) std::_Exit(42);
      });
      ProgressWatch progress; vault_ops::encrypt(childRoot);
      progress.wait({"encrypted", "incomplete"});
      QFAIL("Process did not reach the interruption checkpoint");
    }
    for (const QString phase : {QString("before_encrypt_write"), QString("before_private_publication"), QString("after_encrypt_write"), QString("after_encrypt_metadata"), QString("before_source_claim"), QString("before_claimed_unlink"), QString("after_encrypt_source_removal")}) {
      QTemporaryDir fixture;
      const QString root = real(fixture), original = image(root, "private.png", qRgb(9, 17, 25));
      const QByteArray bytes = readAll(original);
      metadata::setMediaFavorite(original, true);
#ifndef Q_OS_WIN
      xattr::set(original, "user.fixture", QByteArray(2200, 'x'));
#endif
      vault::VaultFile file; file.id = "process-exit-fixture"; file.salt = vault::randomBytes(32); file.state = "encrypting"; file.save(root);
      QProcess child;
      auto environment = QProcessEnvironment::systemEnvironment();
      environment.insert("ROOM237_TEST_EXIT_ROOT", root); environment.insert("ROOM237_TEST_EXIT_PHASE", phase);
      child.setProcessEnvironment(environment);
      child.start(QCoreApplication::applicationFilePath(), {"conversionProcessExitRecovery"});
      QVERIFY(child.waitForFinished(60000));
      QVERIFY2(child.exitCode() == 42, child.readAll().constData());
      vault::unlock(root, fixtureKeys());
      { ProgressWatch progress; vault_ops::encrypt(root); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
      QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
      { ProgressWatch progress; vault_ops::decrypt(root); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("decrypted")); }
      QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
      QCOMPARE(readAll(original), bytes); QVERIFY(metadata::readFileEntry(original)->favorite);
#ifndef Q_OS_WIN
      QCOMPARE(*xattr::get(original, "user.fixture").value, QByteArray(2200, 'x'));
#endif
    }
  }

  void vaultMoveJournalSealsMetadataAndRecovers() {
    QTemporaryDir fixture;
    const QString root = real(fixture), source = path::join(root, "src"), target = path::join(root, "dst");
    createDir(source); createDir(target);
    const QString original = image(source, "private-name.png", qRgb(11, 22, 33));
    const QByteArray bytes = readAll(original);
    metadata::setMediaFavorite(original, true);
    { ProgressWatch progress; vault_ops::setup(root, "long password", {"test"}, {}, {}, {}); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QString name;
    for (const auto &entry : readDir(source)) if (constants::isEncryptedName(entry.name)) name = entry.name;
    QVERIFY(!name.isEmpty());
    QString staging;
    {
      auto journal = media_move::MoveJournal::create(source, target);
      metadata::MoveMetadataUndo undo;
      const auto entry = metadata::readFileEntry(path::join(source, name));
      QVERIFY(entry && entry->favorite);
      undo.source.push_back({name, entry, std::nullopt});
      undo.target.push_back({name, std::nullopt, entry});
      journal->recordMetadata(undo); staging = journal->staging();
      const auto events = readAll(path::join(staging, "events.jsonl"));
      QVERIFY(events.startsWith("enc1:"));
      QVERIFY(!events.contains("private-name")); QVERIFY(!events.contains("favorite"));
    }
    media_move::recoverAlbum(source);
    QVERIFY(!media_move::hasJournal(source));
    QVERIFY(metadata::readFileEntry(path::join(source, name))->favorite);
    const QString revision = metadata::mediaRevision(path::join(source, name));
    QVERIFY_THROWS_EXCEPTION(Error, album::moveMediaBatchWithHook(source, target, {name}, true, [](const QString &, const QString &, const QString &) {}, {{path::join(source, name), "old-revision"}}));
    QVERIFY(isFile(path::join(source, name)));
    QVERIFY(album::moveMediaBatchWithHook(source, target, {name}, true, [](const QString &, const QString &, const QString &) {}, {{path::join(source, name), revision}}).isEmpty());
    { ProgressWatch progress; vault_ops::decrypt(root); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("decrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QCOMPARE(readAll(path::join(target, "private-name.png")), bytes);
    QVERIFY(metadata::readFileEntry(path::join(target, "private-name.png"))->favorite);
  }

  void legacyVaultMigrationAndOversizedAttributesPreserveOriginals_data() {
    QTest::addColumn<bool>("perFile");
    QTest::newRow("album-json") << false;
    QTest::newRow("per-file-meta") << true;
  }
  void legacyVaultMigrationAndOversizedAttributesPreserveOriginals() {
    QFETCH(bool, perFile);
    QTemporaryDir fixture;
    const QString root = real(fixture), original = image(root, "private.png", qRgb(9, 17, 25));
    const QByteArray bytes = readAll(original);
    createDir(metadata::metaDir(root));
    const QString legacyName = perFile ? "private.png.meta" : "album.json";
    const QString legacy = path::join(metadata::metaDir(root), legacyName);
    write(metadata::metaDir(root), legacyName, perFile ? QByteArray(R"({"favorite":true})") : QByteArray(R"({"files":{"private.png":{"favorite":true}},"duplicates_ignore":[]})"));
#ifndef Q_OS_WIN
    xattr::set(original, "user.test", QByteArray(3000, 'a'));
#endif
    ProgressWatch progress;
    vault_ops::setup(root, "long password", {"test"}, {}, {}, {});
#ifndef Q_OS_WIN
    QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("incomplete"));
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QCOMPARE(readAll(original), bytes);
    QVERIFY(exists(legacy));
    QCOMPARE(*xattr::get(original, "user.test").value, QByteArray(3000, 'a'));
    xattr::remove(original, "user.test");
    vault_ops::encrypt(root);
#endif
    QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted"));
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QVERIFY(!exists(legacy));
    vault_ops::decrypt(root);
    QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("decrypted"));
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QCOMPARE(readAll(original), bytes);
    QVERIFY(metadata::readFileEntry(original)->favorite);
  }

  void attributeRestoreFailureKeepsCiphertextForRetry() {
#ifndef Q_OS_LINUX
    QSKIP("Uses the Linux xattr namespace error on disposable files");
#else
    QTemporaryDir fixture;
    const QString root = real(fixture), original = image(root, "private.png", qRgb(9, 17, 25));
    const auto bytes = readAll(original);
    { ProgressWatch progress; vault_ops::setup(root, "long password", {"test"}, {}, {}, {}); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QString sealed;
    for (const auto &entry : readDir(root)) if (constants::isEncryptedName(entry.name)) sealed = path::join(root, entry.name);
    QVERIFY(!sealed.isEmpty());
    const auto keys = vault::keysFor(root);
    auto meta = vault::readMeta(*keys, sealed);
    meta.xattrs.insert("invalid-namespace", QString::fromLatin1(QByteArray("fixture").toBase64()));
    vault::writeMeta(*keys, sealed, meta);
    { ProgressWatch progress; vault_ops::decrypt(root); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("incomplete")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QVERIFY(isFile(sealed)); QVERIFY(exists(path::join(root, vault::kVaultFile)));
    QCOMPARE(readAll(original), bytes);
    meta.xattrs.remove("invalid-namespace"); vault::writeMeta(*keys, sealed, meta);
    { ProgressWatch progress; vault_ops::decrypt(root); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("decrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QCOMPARE(readAll(original), bytes); QVERIFY(!exists(sealed));
#endif
  }

  void corruptedConversionJournalIsRetained() {
    QTemporaryDir fixture;
    const QString root = real(fixture), original = image(root, "private.png", qRgb(9, 17, 25));
    const QByteArray bytes = readAll(original);
    vault::VaultFile file; file.id = "journal-retention"; file.salt = vault::randomBytes(32); file.state = "encrypting"; file.save(root);
    vault::Keys keys; keys.id = file.id; keys.master = vault::Secret(vault::randomBytes(32)); keys.rows = vault::derive(keys.master, "rows"); keys.names = vault::derive(keys.master, "names"); vault::unlock(root, keys);
    const QByteArray damaged("incomplete-record\n");
    const QString journal = write(root, ".room237-vault-journal", damaged);
    { ProgressWatch progress; vault_ops::encrypt(root); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("incomplete")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QCOMPARE(readAll(journal), damaged); QCOMPARE(readAll(original), bytes);
    vault::lock(root);
  }

  void unlockResumesInterruptedRemoval() {
    QTemporaryDir fixture; const QString root = real(fixture);
    const QString original = image(root, "private.png", qRgb(7, 15, 23)); const auto bytes = readAll(original);
    metadata::setMediaFavorite(original, true);
    { ProgressWatch progress; vault_ops::setup(root, "long password", {"test"}, {}, {}, {}); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    FileAccess access(vault::keysFor(root)->filesystem);
    access.setHook([](const QString &phase, const QString &, const QString &) {
      if (phase == "before_claimed_unlink") throw Checkpoint("Synthetic interrupted removal");
    });
    { ProgressWatch progress; vault_ops::decrypt(root); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("incomplete")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    access.setHook({}); vault_ops::lock(root);
    QCOMPARE(vault::VaultFile::load(root)->state, QString("decrypting"));
    { ProgressWatch progress; vault_ops::unlock(root, "long password", "test", {}, {}, {}); QCOMPARE(progress.wait({"decrypted", "incomplete"}), QString("decrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    QVERIFY(!exists(path::join(root, vault::kVaultFile)));
    QCOMPARE(readAll(original), bytes); QVERIFY(metadata::readFileEntry(original)->favorite);
  }

  void queuedRemovalUsesRequestedOperation() {
    QTemporaryDir fixture; const QString root = real(fixture);
    const QString original = image(root, "private.png", qRgb(7, 15, 23)); const auto bytes = readAll(original);
    std::mutex mutex; std::condition_variable changed; bool finished = false; QString finalPhase;
    std::atomic<bool> requested = false;
    host::setEventSink([&](const QString &name, const QJsonValue &payload) {
      if (name != "vault-progress") return;
      const QString phase = payload["phase"].toString();
      if (phase == "encrypted" && !requested.exchange(true)) vault_ops::decrypt(root);
      if (phase == "decrypted" || phase == "incomplete") {
        std::lock_guard lock(mutex); finalPhase = phase; finished = true; changed.notify_all();
      }
    });
    vault_ops::setup(root, "long password", {"test"}, {}, {}, {});
    { std::unique_lock lock(mutex); changed.wait_for(lock, std::chrono::seconds(30), [&] { return finished; }); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(root), 5000);
    host::setEventSink({});
    QVERIFY(finished); QCOMPARE(finalPhase, QString("decrypted"));
    QVERIFY(!exists(path::join(root, vault::kVaultFile))); QCOMPARE(readAll(original), bytes);
  }

  void rootSetupLeaseAndIndependentKeys() {
    QTemporaryDir a, b;
    const QString first = real(a), second = real(b);
    FileAccess access({first});
    auto lease = FileLock::acquireIn(*access.directory(first), ".room237-vault-setup.lock", std::chrono::milliseconds(0), std::chrono::milliseconds(1));
    QVERIFY_THROWS_EXCEPTION(Error, vault_ops::setup(first, "long password", {"test"}, {}, {}, {}));
    QVERIFY(!exists(path::join(first, vault::kVaultFile)));
    lease.reset();
    { ProgressWatch progress; vault_ops::setup(first, "long password", {"test"}, {}, {}, {}); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(first), 5000);
    const auto master = vault::keysFor(first)->master.bytes();
    const auto vaultBytes = readAll(path::join(first, vault::kVaultFile));
    QVERIFY_THROWS_EXCEPTION(Error, vault_ops::setup(first, "other password", {"test"}, {}, {}, {}));
    QCOMPARE(readAll(path::join(first, vault::kVaultFile)), vaultBytes);
    { ProgressWatch progress; vault_ops::setup(second, "long password", {"test"}, {}, {}, {}); QCOMPARE(progress.wait({"encrypted", "incomplete"}), QString("encrypted")); }
    QTRY_VERIFY_WITH_TIMEOUT(!vault_ops::backgroundPassActive(second), 5000);
    QVERIFY(vault::keysFor(second)->master.bytes() != master);
    QVERIFY(!vault::keysFor(first));
    vault::lock(first); QVERIFY(vault::keysFor(second)); vault::lock(second);
  }

  void hardenedRootEncryptsLocksAndRestores() {
    QTemporaryDir dir;
    const QString root = real(dir);
    const QString album = path::join(root, QStringLiteral("Holiday"));
    createDir(album);
    image(root, QStringLiteral("cover-secret.png"), 0xff0000);
    const QString first = image(album, QStringLiteral("beach-secret.png"), 0x00ff00);
    image(album, QStringLiteral("dunes-secret.png"), 0x0000ff);
    const QByteArray original = readAll(first);
    const qint64 mtime = stat(first).mtimeNs;
    metadata::setMediaFavorite(first, true);
    metadata::setMediaTimestamp(album, {QStringLiteral("dunes-secret.png")}, 1234567890);
    catalog::initialize(root);
    catalog::scanAlbum(root, album);

    ProgressWatch progress;
    QVERIFY_THROWS_EXCEPTION(Error, vault_ops::setup(root, QStringLiteral("short"), {QStringLiteral("test")}, {}, {}, {}));
    QVERIFY_THROWS_EXCEPTION(Error, vault_ops::setup(root, QStringLiteral("long password"), {QStringLiteral("test")}, QStringLiteral("long password"), {}, {}));
    const QJsonObject created = vault_ops::setup(root, QStringLiteral("long password"), {QStringLiteral("test")}, QStringLiteral("decoy password"), {}, {});
    const QString recovery = created[QLatin1String("recoveryKey")].toString();
    QVERIFY(vault::parseRecoveryKey(recovery));
    QCOMPARE(progress.wait({QStringLiteral("encrypted"), QStringLiteral("incomplete")}), QStringLiteral("encrypted"));
    QVERIFY2(progress.error.isEmpty(), qPrintable(progress.error));

    // Names, pixels and sealed rows: nothing of the originals is on disk.
    { const QStringList found = leaks(root, "secret"); QVERIFY2(found.isEmpty(), qPrintable(found.join(u", "))); }
    { const QStringList found = leaks(root, original.mid(40, 32)); QVERIFY2(found.isEmpty(), qPrintable(found.join(u", "))); }
    { const QStringList found = leaks(root, "1234567890"); QVERIFY2(found.isEmpty(), qPrintable(found.join(u", "))); }
    QVERIFY(!exists(path::join(path::join(root, catalog::kDirectory), QStringLiteral("index.sqlite"))));
    int sealed = 0;
    for (const QString &name : listDir(album)) sealed += constants::isEncryptedName(name);
    QCOMPARE(sealed, 2);

    // The unlocked root still lists its media by their original names, favorites and dates.
    catalog::initialize(root);
    catalog::scanAlbum(root, album);
    catalog::PageQuery query;
    query.rootDir = root;
    query.albumId = QStringLiteral("Holiday");
    query.sortKey = QStringLiteral("name");
    query.sortDir = QStringLiteral("asc");
    const catalog::MediaPage page = catalog::queryPage(query);
    QCOMPARE(page.total, quint64(2));
    QCOMPARE(page.items[0].media.displayName, std::optional<QString>(QStringLiteral("beach-secret.png")));
    QCOMPARE(page.items[0].media.favorite, std::optional<bool>(true));
    QCOMPARE(unpackFileMeta(page.items[1].media.meta).s, std::optional<qint64>(1234567890));
    const QString sealedFirst = path::join(album, page.items[0].media.name);
    QCOMPARE(vault::readPlain(sealedFirst), original);

    // A folder of the encrypted root opened as a root of its own is empty.
    QVERIFY(vault_ops::status(album)[QLatin1String("nested")].toBool());
    QCOMPARE(album::getAlbumsDetached(album).size(), 0);
    catalog::PageQuery nested;
    nested.rootDir = album;
    nested.sortKey = QStringLiteral("name");
    nested.sortDir = QStringLiteral("asc");
    QCOMPARE(catalog::queryPage(nested).total, quint64(0));
    QVERIFY(!exists(path::join(album, catalog::kDirectory)));

    // Locked: the keys are gone and nothing decrypts until the factor confirms again.
    vault_ops::lock(root);
    QVERIFY(vault_ops::status(root)[QLatin1String("locked")].toBool());
    QVERIFY(vault_ops::status(root)[QLatin1String("resumable")].toBool());
    QVERIFY_THROWS_EXCEPTION(Error, metadata::getFileMetadata(sealedFirst));
    QVERIFY_THROWS_EXCEPTION(Error, catalog::queryPage(query));
    qputenv("ROOM237_QA_FACTOR", "cancel");
    QVERIFY_THROWS_EXCEPTION(Error, vault_ops::resume(root, QStringLiteral("test"), {}, {}));
    qunsetenv("ROOM237_QA_FACTOR");
    vault_ops::resume(root, QStringLiteral("test"), {}, {});
    QVERIFY(!vault_ops::status(root)[QLatin1String("locked")].toBool());
    QCOMPARE(progress.wait({QStringLiteral("encrypted"), QStringLiteral("incomplete")}), QStringLiteral("encrypted"));

    // The password with the recovery key, the decoy password and a wrong one.
    vault_ops::lock(root);
    QVERIFY_THROWS_EXCEPTION(Error, vault_ops::unlock(root, QStringLiteral("wrong password"), QStringLiteral("test"), {}, {}, {}));
    QVERIFY(vault_ops::unlock(root, QStringLiteral("decoy password"), QStringLiteral("test"), {}, {}, {})[QLatin1String("decoy")].toBool());
    QVERIFY(vault_ops::status(root)[QLatin1String("locked")].toBool());
    QVERIFY_THROWS_EXCEPTION(Error, vault_ops::unlock(root, QStringLiteral("long password"), QStringLiteral("recovery"), {}, {}, QStringLiteral("AAAAA")));
    QVERIFY(!vault_ops::unlock(root, QStringLiteral("long password"), QStringLiteral("recovery"), {}, {}, recovery)[QLatin1String("decoy")].toBool());
    QCOMPARE(progress.wait({QStringLiteral("encrypted"), QStringLiteral("incomplete")}), QStringLiteral("encrypted"));
    QVERIFY(vault_ops::verifyPassword(root, QStringLiteral("long password")));
    QVERIFY(!vault_ops::verifyPassword(root, QStringLiteral("decoy password")));

    // A file copied in while hardened is encrypted by the next pass.
    image(album, QStringLiteral("late-secret.png"), 0x777777);
    vault_ops::encrypt(root);
    QCOMPARE(progress.wait({QStringLiteral("encrypted"), QStringLiteral("incomplete")}), QStringLiteral("encrypted"));
    { const QStringList found = leaks(root, "late-secret"); QVERIFY2(found.isEmpty(), qPrintable(found.join(u", "))); }

    // Removing the encryption brings back every name, byte, time, favorite and date.
    vault_ops::decrypt(root);
    QCOMPARE(progress.wait({QStringLiteral("decrypted"), QStringLiteral("incomplete")}), QStringLiteral("decrypted"));
    QVERIFY2(progress.error.isEmpty(), qPrintable(progress.error));
    QVERIFY(!exists(path::join(root, vault::kVaultFile)));
    QCOMPARE(readAll(first), original);
    QCOMPARE(stat(first).mtimeNs, mtime);
    QVERIFY(isFile(path::join(root, QStringLiteral("cover-secret.png"))));
    QVERIFY(isFile(path::join(album, QStringLiteral("late-secret.png"))));
    QVERIFY(metadata::readFileEntry(first)->favorite);
    QCOMPARE(metadata::readEmbeddedTimestamp(path::join(album, QStringLiteral("dunes-secret.png"))), std::optional<qint64>(1234567890));
    for (const QString &name : listDir(album)) QVERIFY2(!constants::isEncryptedName(name), qPrintable(name));
    QVERIFY(!vault_ops::status(root)[QLatin1String("vault")].toBool());
    catalog::initialize(root);
    QCOMPARE(catalog::scanAlbum(root, album), 3);
    QCOMPARE(catalog::queryPage(query).items[0].media.name, QStringLiteral("beach-secret.png"));
  }



  void parallelPassesKeepEveryFile() {
    AdvancedSettings settings = SettingsStore::get();
    settings.preload.encryptWorkers = 8;
    SettingsStore::update(settings);
    QTemporaryDir dir;
    const QString root = real(dir);
    QHash<QString, QByteArray> originals;
    for (const QString &albumName : {QStringLiteral("One"), QStringLiteral("Two")}) {
      const QString album = path::join(root, albumName);
      createDir(album);
      for (int i = 0; i < 20; ++i) {
        const QString file = image(album, QStringLiteral("parallel-%1.png").arg(i), qRgb(i * 11, albumName.size() * 40, 77));
        originals.insert(path::join(albumName, path::fileName(file)), readAll(file));
      }
    }
    ProgressWatch progress;
    vault_ops::setup(root, QStringLiteral("long password"), {QStringLiteral("test")}, {}, {}, {});
    QCOMPARE(progress.wait({QStringLiteral("encrypted"), QStringLiteral("incomplete")}), QStringLiteral("encrypted"));
    QVERIFY2(progress.error.isEmpty(), qPrintable(progress.error));
    int sealed = 0;
    for (const QString &albumName : {QStringLiteral("One"), QStringLiteral("Two")})
      for (const QString &name : listDir(path::join(root, albumName))) {
        QVERIFY2(!name.startsWith(QLatin1String("parallel-")), qPrintable(name));
        sealed += constants::isEncryptedName(name);
      }
    QCOMPARE(sealed, 40);
    vault_ops::decrypt(root);
    QCOMPARE(progress.wait({QStringLiteral("decrypted"), QStringLiteral("incomplete")}), QStringLiteral("decrypted"));
    for (auto it = originals.cbegin(); it != originals.cend(); ++it) QCOMPARE(readAll(path::join(root, it.key())), it.value());
    settings.preload.encryptWorkers = 4;
    SettingsStore::update(settings);
  }

  // Needs a TPM 2.0 or swtpm (TPM2TOOLS_TCTI=swtpm:port=2321) and ROOM237_TEST_TPM=1.
  void tpmSealsUnderThePin() {
    if (!qEnvironmentVariableIsSet("ROOM237_TEST_TPM")) QSKIP("ROOM237_TEST_TPM is not set");
    QString detail;
    QVERIFY2(factors::tpm::available(&detail), qPrintable(detail));
    const factors::Enrollment enrolled = factors::tpm::enroll(QStringLiteral("2468"));
    QCOMPARE(enrolled.secret.bytes().size(), 32);
    QCOMPARE(factors::tpm::secret(enrolled.params, QStringLiteral("2468")).bytes(), enrolled.secret.bytes());
    QVERIFY_THROWS_EXCEPTION(Error, factors::tpm::secret(enrolled.params, QStringLiteral("1357")));
    QVERIFY_THROWS_EXCEPTION(Error, factors::tpm::secret(enrolled.params, QString()));
    // A second sealing is another object with another secret.
    const factors::Enrollment other = factors::tpm::enroll(QStringLiteral("2468"));
    QVERIFY(other.secret.bytes() != enrolled.secret.bytes());
    QJsonObject damaged = enrolled.params;
    QByteArray blob = QByteArray::fromBase64(damaged.value(QLatin1String("private")).toString().toLatin1());
    blob[blob.size() - 3] = char(blob[blob.size() - 3] ^ 1);
    damaged[QStringLiteral("private")] = QString::fromLatin1(blob.toBase64());
    QVERIFY_THROWS_EXCEPTION(Error, factors::tpm::secret(damaged, QStringLiteral("2468")));

    // The whole vault with the TPM as its factor.
    QTemporaryDir dir;
    const QString root = real(dir);
    image(root, QStringLiteral("tpm-secret.png"), 0x123456);
    ProgressWatch progress;
    vault_ops::setup(root, QStringLiteral("long password"), {QStringLiteral("tpm2")}, {}, {}, QStringLiteral("2468"));
    QCOMPARE(progress.wait({QStringLiteral("encrypted"), QStringLiteral("incomplete")}), QStringLiteral("encrypted"));
    QVERIFY(vault_ops::status(root)[QLatin1String("pinFactors")].toArray().contains(QStringLiteral("tpm2")));
    vault_ops::lock(root);
    QVERIFY_THROWS_EXCEPTION(Error, vault_ops::resume(root, QStringLiteral("tpm2"), {}, QStringLiteral("0000")));
    vault_ops::resume(root, QStringLiteral("tpm2"), {}, QStringLiteral("2468"));
    QVERIFY(!vault_ops::status(root)[QLatin1String("locked")].toBool());
    progress.wait({QStringLiteral("encrypted"), QStringLiteral("incomplete")});
    vault_ops::lock(root);
    QVERIFY(!vault_ops::unlock(root, QStringLiteral("long password"), QStringLiteral("tpm2"), {}, QStringLiteral("2468"), {})[QLatin1String("decoy")].toBool());
    progress.wait({QStringLiteral("encrypted"), QStringLiteral("incomplete")});
  }

  // --- Paths ----------------------------------------------------------------------------------------------------

  void pathsFollowComponentRules() {
    QCOMPARE(path::parent(QStringLiteral("/a/b/")), QStringLiteral("/a"));
    QCOMPARE(path::parent(QStringLiteral("/a")), QStringLiteral("/"));
    QCOMPARE(path::fileName(QStringLiteral("/a/b.jpg")), QStringLiteral("b.jpg"));
    QCOMPARE(path::extension(QStringLiteral(".hidden")), QString());
    QCOMPARE(path::extension(QStringLiteral("a.tar.gz")), QStringLiteral("gz"));
    QVERIFY(path::startsWith(QStringLiteral("/a/bc"), QStringLiteral("/a")));
    QVERIFY(!path::startsWith(QStringLiteral("/ab"), QStringLiteral("/a")));
    QVERIFY(!validFileName(QStringLiteral("a/b")));
    QVERIFY(!validFileName(QStringLiteral("..")));
  }

private:
  std::unique_ptr<QTemporaryDir> profile_;
};

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
  _setmode(_fileno(stdout), _O_BINARY);  // The mock PNG producer must have the same binary pipe contract as FFmpeg.
#endif
  if (argc == 2 && QByteArray(argv[1]) == "--security-output-helper") {
    for (int i = 0; i < 4096; ++i) {
      const QByteArray out(65536, i < 64 ? 'O' : 'o'), err(65536, i < 64 ? 'E' : 'e');
      if (fwrite(out.constData(), 1, size_t(out.size()), stdout) != size_t(out.size())) return 1;
      if (fwrite(err.constData(), 1, size_t(err.size()), stderr) != size_t(err.size())) return 1;
    }
    return 0;
  }
  if (argc == 2 && QByteArray(argv[1]) == "--security-png-helper") {
    char bytes[4096]; while (fread(bytes, 1, sizeof(bytes), stdin)) {}
    const QByteArray err(65536, 'E');
    for (int i = 0; i < 96; ++i) fwrite(err.constData(), 1, size_t(err.size()), stderr);
    QImage image(1280, 1280, QImage::Format_RGBA8888);
    quint32 state = 1;
    for (qsizetype i = 0; i < image.sizeInBytes(); ++i) { state = state * 1664525u + 1013904223u; image.bits()[i] = uchar(state >> 24); }
    QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return 2;
    return fwrite(png.constData(), 1, size_t(png.size()), stdout) == size_t(png.size()) ? 0 : 3;
  }
  QCoreApplication application(argc, argv);
  if (argc == 3 && QByteArray(argv[1]) == "--security-read-file") {
    QFile input(QString::fromLocal8Bit(argv[2]));
    if (!input.open(QIODevice::ReadOnly)) return 2;
    const QByteArray bytes = input.readAll();
    if (!input.seek(bytes.size() / 2) || input.readAll() != bytes.mid(bytes.size() / 2) || !input.seek(0) || input.readAll() != bytes) return 4;
    return fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) == size_t(bytes.size()) ? 0 : 3;
  }
  if (argc == 2 && QByteArray(argv[1]) == "--security-idle-helper") { QThread::msleep(1000); return 0; }
  if (argc == 2 && QByteArray(argv[1]) == "--security-memory-probe") {
#ifndef Q_OS_WIN
    rusage before{}, after{}; getrusage(RUSAGE_SELF, &before);
#endif
    const auto result = runProcess(QCoreApplication::applicationFilePath(), {"--security-output-helper"}, 30000);
#ifndef Q_OS_WIN
    getrusage(RUSAGE_SELF, &after);
#ifdef Q_OS_MACOS
    const qint64 growth = after.ru_maxrss - before.ru_maxrss;
#else
    const qint64 growth = (after.ru_maxrss - before.ru_maxrss) * 1024;
#endif
    fprintf(stderr, "peak RSS growth: %lld bytes", (long long)growth);
    if (growth > (64 << 20)) return 4;
#endif
    return result.success && result.stdoutBytes.size() == (4 << 20) && result.stderrBytes.size() == (4 << 20) ? 0 : 5;
  }
  CoreTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "core_tests.moc"
