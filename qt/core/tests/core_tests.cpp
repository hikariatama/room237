// Unit tests of the library core: on-disk formats shared with earlier releases, crash recovery of journaled media
// operations, update verification and unpacking.
#include "album.h"
#include "album_db.h"
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

class CoreTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void initTestCase() {
    profile_.reset(new QTemporaryDir);
    SettingsStore::load(profile_->path());
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

QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
