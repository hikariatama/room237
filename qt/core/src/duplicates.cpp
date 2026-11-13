#include "duplicates.h"

#include "album_db.h"
#include "image_hash.h"
#include "metadata.h"
#include "thumb.h"
#include "util.h"

#include <bit>

namespace core::duplicates {

namespace {
std::optional<Bits> decodeHashBitsBase64(const QString &text, quint32 expectedBits) {
  const auto decoded = fromBase64(text.toLatin1());
  if (!decoded) return std::nullopt;
  const QByteArray bytes = *decoded;
  if (qsizetype(expectedBits + 7) / 8 < bytes.size()) return std::nullopt;
  if (size_t(bytes.size()) * 8 != expectedBits || bytes.size() % 8 != 0) return std::nullopt;
  Bits out;
  for (qsizetype i = 0; i < bytes.size(); i += 8) {
    quint64 word = 0;
    for (int b = 0; b < 8; ++b) word |= quint64(quint8(bytes[i + b])) << (8 * b);
    out.push_back(word);
  }
  return out;
}

std::optional<Bits> loadCachedBits(const FileMetaEntry &entry, const QString &path, const HashConfig &config) {
  if (entry.hashFailed || !metadata::hashCacheCurrent(path, entry)) return std::nullopt;
  if (entry.hashVersion != config.hashVersion) return std::nullopt;
  if (entry.hashBits && *entry.hashBits != config.bits) return std::nullopt;
  if (!entry.hash) return std::nullopt;
  return decodeHashBitsBase64(*entry.hash, config.bits);
}

std::optional<Bits> toChunks(const QByteArray &bytes) {
  if (bytes.size() % 8 != 0) return std::nullopt;
  Bits out;
  for (qsizetype i = 0; i < bytes.size(); i += 8) {
    quint64 word = 0;
    for (int b = 0; b < 8; ++b) word |= quint64(quint8(bytes[i + b])) << (8 * b);
    out.push_back(word);
  }
  return out;
}

std::optional<Bits> computeWithCachedBits(const QString &p, const HashConfig &config, const AdvancedSettings &settings, std::optional<Bits> cached) {
  if (cached) return cached;
  const auto source = metadata::sourceRevision(p);
  if (!source) return std::nullopt;
  const QString parent = path::parent(p);
  if (parent.isEmpty()) return std::nullopt;
  const QString thumbDir = path::join(parent, thumb::kThumbDir);
  std::optional<QString> lastError;
  auto tryHashPath = [&](const QString &target) -> Bits {
    if (metadata::sourceRevision(p) != source) fail(QStringLiteral("Source changed during hash computation"));
    const QString revision = metadata::mediaRevision(p);
    const QByteArray hash = image_hash::hash(image_hash::loadImage(target, config), config);
    auto bits = toChunks(hash);
    if (!bits) fail(QStringLiteral("unexpected hash length"));
    try {
      metadata::writeAlbumFileHash(p, QString::fromLatin1(hash.toBase64()), config.hashVersion, config.bits, revision);
    } catch (const Error &e) {
      qWarning("Failed to persist hash for %s: %s", qUtf8Printable(p), qUtf8Printable(e.message()));
      throw;
    }
    return *bits;
  };
  auto attemptThumb = [&]() -> std::optional<Bits> {
    try {
      return tryHashPath(thumb::ensureThumbWithSettings(p, thumbDir, settings));
    } catch (const std::exception &e) {
      lastError = QString::fromUtf8(e.what());
      return std::nullopt;
    }
  };
  auto attemptOriginal = [&]() -> std::optional<Bits> {
    try {
      return tryHashPath(p);
    } catch (const std::exception &e) {
      lastError = QString::fromUtf8(e.what());
      return std::nullopt;
    }
  };
  std::optional<Bits> result = config.useThumbnailsFirst ? attemptThumb() : attemptOriginal();
  if (!result) result = config.useThumbnailsFirst ? attemptOriginal() : attemptThumb();
  if (!result && metadata::sourceRevision(p) == source) {
    if (lastError) qWarning("Failed to hash %s: %s", qUtf8Printable(p), qUtf8Printable(*lastError));
    try {
      metadata::markHashFailedIfCurrent(p, metadata::mediaRevision(p), config, settings.processingVersion());
    } catch (const std::exception &) {
    }
  }
  return result;
}
}  // namespace

bool withinThreshold(const quint64 *a, const quint64 *b, size_t words, quint32 threshold) {
  quint32 distance = 0;
  for (size_t i = 0; i < words; ++i) {
    distance += quint32(std::popcount(a[i] ^ b[i]));
    if (distance > threshold) return false;
  }
  return distance <= threshold;
}

std::optional<Bits> computeHashForPath(const AlbumMeta &meta, const QString &p, const HashConfig &config, const AdvancedSettings &settings) {
  const QString name = path::fileName(p);
  if (name.isEmpty()) return std::nullopt;
  std::optional<Bits> cached;
  if (auto it = meta.files.constFind(name); it != meta.files.cend()) cached = loadCachedBits(*it, p, config);
  return computeWithCachedBits(p, config, settings, cached);
}

std::optional<Bits> computeHashForFile(const QString &p, const HashConfig &config, const AdvancedSettings &settings) {
  std::optional<FileMetaEntry> entry;
  try {
    entry = metadata::readFileEntry(p);
  } catch (const Error &error) {
    qWarning("Cannot read hash metadata for %s: %s", qUtf8Printable(p), qUtf8Printable(error.message()));
    return std::nullopt;
  }
  return computeHashFromEntry(p, config, settings, entry);
}

std::optional<Bits> computeHashFromEntry(const QString &p, const HashConfig &config, const AdvancedSettings &settings, const std::optional<FileMetaEntry> &entry) {
  return computeWithCachedBits(p, config, settings, entry ? loadCachedBits(*entry, p, config) : std::nullopt);
}

void markNonDuplicates(const QString &dir, const QStringList &files) {
  if (!isDir(dir)) fail(QStringLiteral("%1 is not a directory").arg(dir));
  if (files.size() < 2) return;
  for (const QString &file : files) validateFileName(file);
  metadata::prepareDatabase(dir);
  auto guard = metadata::lockMetadataForWrite({dir});
  album_db::ignorePairs(path::join(dir, QStringLiteral(".room237-metadata/album.sqlite")), files);
}

}  // namespace core::duplicates
