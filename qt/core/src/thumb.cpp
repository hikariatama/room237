#include "thumb.h"

#include "album_tree.h"
#include "constants.h"
#include "metadata.h"
#include "native_image.h"
#include "util.h"
#include "vault.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSet>
#include <condition_variable>

namespace core::thumb {

namespace {
QString hashedThumbName(const QString &name) { return QStringLiteral("room237-preview-v1-%1.webp").arg(sha256Hex(name.toUtf8())); }

// Previews this process is generating. A worker and a foreground request for the same preview wait for each other
// instead of decoding twice.
std::mutex generatingMutex;
std::condition_variable &generatingReleased = *new std::condition_variable;  // Never destroyed; workers may wait.
QSet<QString> generating;

class GenerationGuard {
public:
  GenerationGuard(const QString &thumb, std::chrono::milliseconds timeout) : thumb_(thumb) {
    std::unique_lock lock(generatingMutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (generating.contains(thumb)) {
      if (generatingReleased.wait_until(lock, deadline) == std::cv_status::timeout && generating.contains(thumb)) fail(QStringLiteral("Thumbnail is still being generated: %1").arg(thumb));
    }
    generating.insert(thumb);
  }
  ~GenerationGuard() {
    {
      std::lock_guard lock(generatingMutex);
      generating.remove(thumb_);
    }
    generatingReleased.notify_all();
  }
private:
  QString thumb_;
};

QStringList scaleFilter(quint32 maxDim) { return {QStringLiteral("-vf"), QStringLiteral("scale=min(iw\\,%1):min(ih\\,%1):force_original_aspect_ratio=decrease").arg(maxDim)}; }

// This output shares the original decode with the regular preview.
QStringList blurOutput(const QString &output) {
  return {QStringLiteral("-map"), QStringLiteral("0:v:0"), QStringLiteral("-vf"), QStringLiteral("scale=16:16,format=yuv420p"), QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-an"), QStringLiteral("-c:v"), QStringLiteral("libwebp"), QStringLiteral("-q:v"), QStringLiteral("20"), QStringLiteral("-compression_level"), QStringLiteral("0"), output};
}

bool runFfmpeg(const QStringList &args, const AdvancedSettings &settings, const QString &what, const QString &input, const QString &output) {
  ProcessResult result;
  try {
    result = runProcess(ffmpegPath(), args, int(settings.ffmpeg.timeoutSecs * 1000));
  } catch (const Error &error) {
    qWarning("%s timeout %s→%s: %s", qUtf8Printable(what), qUtf8Printable(input), qUtf8Printable(output), qUtf8Printable(error.message()));
    throw;
  }
  if (!result.success) qWarning("%s failed %s→%s", qUtf8Printable(what), qUtf8Printable(input), qUtf8Printable(output));
  return result.success;
}

QStringList tuning(bool isVideo, const AdvancedSettings &settings) {
  QStringList args = ffmpegCommandPrefix();
  args << ffmpegTuning(isVideo, settings.ffmpeg);
  return args;
}

bool generateImageThumbnail(const QString &input, const QString &output, const QString &blurOutputPath, const AdvancedSettings &settings) {
  DecodePermit permit;
  if (native_image::thumbnailsEnabled()) {
    if (auto preview = native_image::thumbnail(input, settings.thumbnails.maxDim, float(settings.thumbnails.imageWebpQuality), int(settings.thumbnails.imageWebpCompressionLevel))) {
      writeAll(output, preview->main);
      writeAll(blurOutputPath, preview->blur);
      return true;
    }
  }
  QStringList args = tuning(false, settings);
  args << QStringLiteral("-i") << input << QStringLiteral("-y") << QStringLiteral("-frames:v") << QStringLiteral("1") << scaleFilter(settings.thumbnails.maxDim) << QStringLiteral("-c:v") << QStringLiteral("libwebp") << QStringLiteral("-q:v")
       << QString::number(settings.thumbnails.imageWebpQuality) << QStringLiteral("-compression_level") << QString::number(settings.thumbnails.imageWebpCompressionLevel) << output << blurOutput(blurOutputPath);
  return runFfmpeg(args, settings, QStringLiteral("thumb image"), input, output);
}

bool generateVideoThumbnailAt(const QString &input, const QString &output, const QString &blurOutputPath, const AdvancedSettings &settings, float seek) {
  DecodePermit permit;
  QStringList args = tuning(true, settings);
  args << QStringLiteral("-ss") << QString::asprintf("%.3f", double(seek)) << QStringLiteral("-i") << input << QStringLiteral("-y") << QStringLiteral("-frames:v") << QStringLiteral("1") << scaleFilter(settings.thumbnails.maxDim) << output << blurOutput(blurOutputPath);
  const bool ok = runFfmpeg(args, settings, QStringLiteral("thumb video"), input, output);
  return ok && stat(output).size > 0;
}

bool generateVideoThumbnail(const QString &input, const QString &output, const QString &blurOutputPath, const AdvancedSettings &settings) {
  const float seek = std::max(settings.thumbnails.videoSeekSeconds, 0.0f);
  if (generateVideoThumbnailAt(input, output, blurOutputPath, settings, seek)) return true;
  // A valid clip can end before the configured seek position. Retry only once, and never restart a timed-out
  // process (a timeout throws).
  if (seek > 0.0f) return generateVideoThumbnailAt(input, output, blurOutputPath, settings, 0.0f);
  return false;
}

QString readBlurPreview(const QString &path) {
  const QByteArray bytes = readAll(path, 4097);
  if (bytes.size() > 4096) fail(QStringLiteral("Blur preview exceeds 4 KiB"));
  QByteArray rgba;
  int width = 0, height = 0;
  if (!native_image::decodeWebp(bytes, rgba, width, height)) fail(QStringLiteral("Invalid WebP blur preview"));
  if (width != 16 || height != 16) fail(QStringLiteral("Blur preview must be 16 by 16 pixels"));
  return QString::fromLatin1(bytes.toBase64());
}

// Upgrade existing libraries from their small cached preview, without reading or decoding the original again.
QString backfillBlurPreview(const QString &thumb, const AdvancedSettings &settings) {
  const QString dir = path::parent(thumb);
  if (dir.isEmpty()) fail(QStringLiteral("Missing preview directory"));
  TempFile temporary(dir, kTempPrefix, QStringLiteral(".webp"));
  DecodePermit permit;
  QStringList args = tuning(false, settings);
  args << QStringLiteral("-y") << QStringLiteral("-i") << thumb << blurOutput(temporary.path());
  if (!runProcess(ffmpegPath(), args, int(settings.ffmpeg.timeoutSecs * 1000)).success) fail(QStringLiteral("Blur preview generation failed"));
  return readBlurPreview(temporary.path());
}

}  // namespace

// ctime detects same-size edits with a restored mtime on Unix. The hash is streamed only after this identity
// changes, never because time has elapsed.
std::optional<QString> contentRevision(const QString &path) {
  const FileInfo info = stat(path);
  auto revision = metadata::sourceRevisionFromInfo(info);
  if (!revision) return std::nullopt;
#ifndef Q_OS_WIN
  return *revision + QStringLiteral("-%1-%2").arg(info.ctimeSec).arg(info.ctimeNsec);
#else
  return revision;
#endif
}

namespace {
// Keep sequential source reads bounded even when many workers are configured.
Budget &sourceReaders = *new Budget(2);  // Never destroyed; workers may wait.
}  // namespace
QString sourceHash(const QString &path) {
  // An encrypted file names its plaintext digest; reading the ciphertext again would only repeat that work.
  if (const auto keys = vault::keysFor(path); keys && vault::isEncryptedFile(path)) return QStringLiteral("vault:") + toHex(vault::readMeta(*keys, path).sha256);
  Budget::Permit permit(sourceReaders);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
  QCryptographicHash hasher(QCryptographicHash::Sha256);
  QByteArray buffer;
  while (!(buffer = file.read(64 * 1024)).isEmpty()) hasher.addData(buffer);
  if (file.error() != QFileDevice::NoError) fail(file.errorString());
  return toHex(hasher.result());
}

QString maintenanceLockPath(const QString &thumbDir) { return path::join(thumbDir, QStringLiteral(".room237-maintenance.lock")); }

QString thumbPath(const QString &original, const QString &thumbDir) {
  const QString name = path::fileName(original);
  if (name.isEmpty()) fail(QStringLiteral("missing filename"));
  const QString legacy = path::join(thumbDir, name + QStringLiteral(".webp"));
  // Leave existing previews usable on filesystems with a larger name limit. The name must also fit a 255-byte
  // component on other filesystems.
  if (name.toUtf8().size() <= 250 || isFile(legacy)) return legacy;
  return path::join(thumbDir, hashedThumbName(name));
}

QStringList deletedThumbnailPaths(const QString &name, const QString &thumbDir) {
  QStringList paths{path::join(thumbDir, name + QStringLiteral(".webp"))};
  if (name.toUtf8().size() > 250) paths << path::join(thumbDir, hashedThumbName(name));
  return paths;
}

std::optional<QString> detachedThumbnailName(const QString &original) {
  const QString originalName = path::fileName(original);
  if (originalName.isEmpty() || originalName.toUtf8().size() <= 250) return std::nullopt;
  const QString parent = path::parent(original);
  if (parent.isEmpty()) return std::nullopt;
  QString preview;
  try {
    preview = thumbPath(original, path::join(parent, kThumbDir));
  } catch (const Error &) {
    return std::nullopt;
  }
  const QString name = path::fileName(preview);
  if (name == originalName + QStringLiteral(".webp")) return std::nullopt;
  return name;
}

bool isLegacyPreviewLock(const QString &name) { return name.endsWith(QLatin1String(".lock")) && !name.startsWith(QLatin1String(".room237-")); }

// Versions up to 7.0 left an empty "<preview>.lock" beside every preview. They are removed once per album and run,
// under the exclusive directory lease so that no generator of an older version is still using one.
void removeLegacyPreviewLocks(const QString &thumbDir) {
  static std::mutex mutex;
  static QSet<QString> cleaned;
  {
    std::lock_guard lock(mutex);
    if (cleaned.contains(thumbDir)) return;
  }
  std::unique_ptr<FileLock> lease;
  try {
    lease = FileLock::tryAcquire(maintenanceLockPath(thumbDir));
  } catch (const Error &) {
    return;
  }
  if (!lease) return;
  std::vector<DirEntry> entries;
  try {
    entries = readDir(thumbDir);
  } catch (const Error &) {
    return;
  }
  for (const DirEntry &entry : entries)
    if (isLegacyPreviewLock(entry.name)) {
      try {
        removeFile(path::join(thumbDir, entry.name));
      } catch (const Error &) {
      }
    }
  std::lock_guard lock(mutex);
  cleaned.insert(thumbDir);
}

bool cachedPreviewsCurrent(const QString &path, const QString &thumbDir, const FileMetaEntry &entry, const QString &version) { return entry.blurPreview && cachedThumbCurrent(path, thumbDir, entry, version); }

bool cachedThumbCurrent(const QString &path, const QString &thumbDir, const FileMetaEntry &entry, const QString &version) {
  if (entry.thumbVersion != version || !entry.thumbSourceHash || !entry.thumbHashRevision || entry.thumbHashRevision != contentRevision(path)) return false;
  try {
    return isFile(thumbPath(path, thumbDir));
  } catch (const Error &) {
    return false;
  }
}

QString ensureThumb(const QString &path, const QString &thumbDir) { return ensureThumbWithSettings(path, thumbDir, readSettings()); }

namespace {
// An encrypted original is decoded from memory and its preview is sealed before it reaches the disk.
std::pair<QByteArray, QByteArray> generateSealedSources(const vault::Keys &keys, const QString &path, const AdvancedSettings &settings) {
  const vault::FileMeta meta = vault::readMeta(keys, path);
  const QString ext = constants::lowerExtension(meta.name);
  DecodePermit permit;
  if (constants::isImageExtension(ext)) {
    const QByteArray plain = vault::decryptAll(keys, path, qint64(1) << 30);
    if (auto preview = native_image::thumbnailFromData(plain, settings.thumbnails.maxDim, float(settings.thumbnails.imageWebpQuality), int(settings.thumbnails.imageWebpCompressionLevel))) return {preview->main, preview->blur};
  }
  // Video, or an image format ImageIO and Qt cannot decode: FFmpeg reads the plaintext from a pipe.
  vault::DecryptingDevice input(std::make_shared<vault::Keys>(keys), path);
  if (!input.open(QIODevice::ReadOnly)) fail(input.errorString());
  QStringList args = tuning(constants::isVideoExtension(ext), settings);
  if (constants::isVideoExtension(ext)) args << QStringLiteral("-ss") << QString::asprintf("%.3f", double(std::max(settings.thumbnails.videoSeekSeconds, 0.0f)));
  args << QStringLiteral("-i") << QStringLiteral("pipe:0") << QStringLiteral("-y") << QStringLiteral("-frames:v") << QStringLiteral("1") << scaleFilter(settings.thumbnails.maxDim) << QStringLiteral("-c:v") << QStringLiteral("libwebp") << QStringLiteral("-q:v")
       << QString::number(settings.thumbnails.imageWebpQuality) << QStringLiteral("-compression_level") << QString::number(settings.thumbnails.imageWebpCompressionLevel) << QStringLiteral("-f") << QStringLiteral("webp") << QStringLiteral("pipe:1");
  const ProcessResult result = runWithInput(ffmpegPath(), args, input, int(settings.ffmpeg.timeoutSecs * 1000) * 4);
  if (!result.success || result.stdoutBytes.isEmpty()) fail(QStringLiteral("Thumbnail generation failed: %1").arg(path));
  const auto blur = native_image::blurFromImage(result.stdoutBytes);
  if (!blur) fail(QStringLiteral("Blur preview generation failed"));
  return {result.stdoutBytes, *blur};
}
QString blurFromBytes(const QByteArray &bytes) {
  if (bytes.size() > 4096) fail(QStringLiteral("Blur preview exceeds 4 KiB"));
  QByteArray rgba;
  int width = 0, height = 0;
  if (!native_image::decodeWebp(bytes, rgba, width, height) || width != 16 || height != 16) fail(QStringLiteral("Blur preview must be 16 by 16 pixels"));
  return QString::fromLatin1(bytes.toBase64());
}
}  // namespace

QString ensureThumbWithSettings(const QString &path, const QString &thumbDir, const AdvancedSettings &settings) {
  QString album = path::parent(path);
  if (album.isEmpty()) album = QStringLiteral(".");
  auto tree = album_tree::read({album});
  const QString thumb = thumbPath(path, thumbDir);
  const QString version = settings.thumbVersion();
  if (auto entry = metadata::readFileEntry(path); entry && cachedPreviewsCurrent(path, thumbDir, *entry, version)) return thumb;
  bool inVault = false;
  const auto keys = vault::keysFor(path, &inVault);
  if (inVault && !keys) fail(QStringLiteral("This library is locked"));

  createDerivedDir(thumbDir);
  const auto lockTimeout = std::chrono::milliseconds((settings.ffmpeg.timeoutSecs * 2 + 5) * 1000);
  // Generators can run concurrently; maintenance needs exclusive ownership before removing abandoned temporary files.
  // Take this before the generation guard.
  auto directoryLease = FileLock::acquireShared(maintenanceLockPath(thumbDir), lockTimeout, std::chrono::milliseconds(settings.thumbnails.lockPollMs));
  tree->ensureCurrent({album});
  GenerationGuard generation(thumb, lockTimeout);
  const std::optional<FileMetaEntry> cached = metadata::readFileEntry(path);
  if (cached && cachedThumbCurrent(path, thumbDir, *cached, version)) {
    if (cached->blurPreview) return thumb;
    const auto source = metadata::sourceRevision(path);
    if (!source) fail(QStringLiteral("Source media is unavailable"));
    QString blur;
    if (keys && vault::isEncryptedFile(thumb)) {
      const auto small = native_image::blurFromImage(vault::decryptAll(*keys, thumb, 64 << 20));
      if (!small) fail(QStringLiteral("Blur preview generation failed"));
      blur = blurFromBytes(*small);
    } else {
      blur = backfillBlurPreview(thumb, settings);
    }
    metadata::writeThumbState(path, version, *source, cached->thumbSourceHash, cached->thumbHashRevision, blur);
    return thumb;
  }
  const auto source = metadata::sourceRevision(path);
  if (!source) fail(QStringLiteral("Source media is unavailable"));
  const auto hashRevision = contentRevision(path);
  if (!hashRevision) fail(QStringLiteral("Source media is unavailable"));
  const QString hash = sourceHash(path);
  if (contentRevision(path) != hashRevision) fail(QStringLiteral("Media changed while hashing thumbnail source"));
  // A timestamp, inode or external attribute change alone needs no decoding.
  if (isFile(thumb) && cached && cached->thumbVersion == version && cached->thumbSourceHash == hash && (cached->blurPreview || !keys)) {
    const QString blur = cached->blurPreview ? *cached->blurPreview : backfillBlurPreview(thumb, settings);
    metadata::writeThumbState(path, version, *source, hash, hashRevision, blur);
    return thumb;
  }
  if (keys && vault::isEncryptedFile(path)) {
    const auto [main, small] = generateSealedSources(*keys, path, settings);
    if (contentRevision(path) != hashRevision) fail(QStringLiteral("Media changed during thumbnail generation"));
    const QString blur = blurFromBytes(small);
    vault::encryptBytes(*keys, main, thumb, vault::FileMeta{path::fileName(thumb)}, true);
    metadata::writeThumbState(path, version, *source, hash, hashRevision, blur);
    return thumb;
  }
  TempFile temporary(thumbDir, kTempPrefix, QStringLiteral(".webp"));
  TempFile blurTemp(thumbDir, kTempPrefix, QStringLiteral(".webp"));
  const QString ext = constants::lowerExtension(path);
  bool generated = false;
  if (constants::isImageExtension(ext)) generated = generateImageThumbnail(path, temporary.path(), blurTemp.path(), settings);
  else if (constants::isVideoExtension(ext)) generated = generateVideoThumbnail(path, temporary.path(), blurTemp.path(), settings);
  if (!generated || temporary.size() == 0) fail(QStringLiteral("Thumbnail generation failed: %1").arg(path));
  if (contentRevision(path) != hashRevision) fail(QStringLiteral("Media changed during thumbnail generation"));
  const QString blur = readBlurPreview(blurTemp.path());
  temporary.persist(thumb);
  metadata::writeThumbState(path, version, *source, hash, hashRevision, blur);
  return thumb;
}

}  // namespace core::thumb
