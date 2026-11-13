#include "settings.h"

#include "fs.h"
#include "json.h"
#include "preload.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace core {



namespace {
const char *const kHashSchemaVersion = "1";
std::shared_mutex settingsMutex;
std::optional<AdvancedSettings> current;
QString settingsPath;

const char *hashSizeName(HashSize size) {
  switch (size) {
  case HashSize::S8x8: return "8x8";
  case HashSize::S32x32: return "32x32";
  default: return "16x16";
  }
}
const char *algName(HashAlgorithm alg) {
  switch (alg) {
  case HashAlgorithm::Phash: return "phash";
  case HashAlgorithm::Dhash: return "dhash";
  default: return "blockhash";
  }
}
const char *algDebug(HashAlgorithm alg) {
  switch (alg) {
  case HashAlgorithm::Phash: return "Phash";
  case HashAlgorithm::Dhash: return "Dhash";
  default: return "Blockhash";
  }
}
const char *filterName(ResizeFilter filter) {
  switch (filter) {
  case ResizeFilter::Triangle: return "triangle";
  case ResizeFilter::Catmullrom: return "catmullrom";
  case ResizeFilter::Lanczos3: return "lanczos3";
  default: return "nearest";
  }
}
const char *filterDebug(ResizeFilter filter) {
  switch (filter) {
  case ResizeFilter::Triangle: return "Triangle";
  case ResizeFilter::Catmullrom: return "Catmullrom";
  case ResizeFilter::Lanczos3: return "Lanczos3";
  default: return "Nearest";
  }
}
template <typename E, size_t N> E parseEnum(const QJsonValue &value, const char *field, const std::pair<const char *, E> (&names)[N]) {
  const QString text = json::stringField(value, field);
  for (const auto &[name, item] : names)
    if (text == QLatin1String(name)) return item;
  fail(QStringLiteral("%1: unknown variant `%2`").arg(QLatin1String(field), text));
}

QJsonValue at(const QJsonObject &object, const char *key) { return object.value(QLatin1String(key)); }
QJsonObject section(const QJsonObject &object, const char *key, bool &present) {
  const QJsonValue value = at(object, key);
  present = !value.isUndefined() && !value.isNull();
  if (value.isUndefined()) return {};
  if (!value.isObject()) fail(QStringLiteral("%1: invalid type, expected struct").arg(QLatin1String(key)));
  return value.toObject();
}

QString threadsJson(const ThreadSetting &threads) { return threads.automatic ? json::string(QStringLiteral("auto")) : QString::number(threads.count); }

json::Object duplicatesJson(const DuplicatesSettings &d) {
  return std::move(json::Object().unum("threshold", d.threshold).str("hashSize", QString::fromLatin1(hashSizeName(d.hashSize))).str("hashAlg", QString::fromLatin1(algName(d.hashAlg))).str("resizeFilter", QString::fromLatin1(filterName(d.resizeFilter))).flag("useThumbnailsFirst", d.useThumbnailsFirst).unum("maxFilesPerAlbum", d.maxFilesPerAlbum));
}
json::Object thumbnailsJson(const ThumbnailSettings &t) {
  return std::move(json::Object().unum("maxDim", t.maxDim).unum("imageWebpQuality", t.imageWebpQuality).unum("imageWebpCompressionLevel", t.imageWebpCompressionLevel).raw("videoSeekSeconds", json::float32(t.videoSeekSeconds)).unum("lockPollMs", t.lockPollMs));
}
json::Object ffmpegJson(const FfmpegSettings &f) {
  return std::move(json::Object().raw("threads", threadsJson(f.threads)).unum("timeoutSecs", f.timeoutSecs).str("hwaccel", f.hwaccel).unum("processWaitPollMs", f.processWaitPollMs));
}
json::Object metadataJson(const MetadataSettings &m) {
  return std::move(json::Object().raw("ffmpegProbeTimeoutSecs", m.ffmpegProbeTimeoutSecs ? QString::number(*m.ffmpegProbeTimeoutSecs) : QStringLiteral("null")).flag("parseCreationTime", m.parseCreationTime));
}
}  // namespace

int hashSide(HashSize size) { return size == HashSize::S8x8 ? 8 : size == HashSize::S32x32 ? 32 : 16; }

int ThreadSetting::resolved() const {
  if (!automatic) return std::clamp(count, 1, 32);
  const unsigned n = std::thread::hardware_concurrency();
  return n ? std::clamp(int(n), 1, 32) : 4;
}

quint32 DuplicatesSettings::effectiveThreshold() const {
  const double bits = double(hashSide(hashSize) * hashSide(hashSize));
  const double scaled = std::round(double(threshold) * (bits / 256.0));
  return quint32(std::min(std::max(std::clamp(scaled, 0.0, std::max(bits, 1.0)), 0.0), bits));
}

AdvancedSettings AdvancedSettings::fromJson(const QJsonObject &o) {
  AdvancedSettings s;
  bool present = false;
  // Nested structs have no defaults: a section that is present must be complete, as before.
  QJsonObject d = section(o, "duplicates", present);
  if (present) {
    s.duplicates.threshold = quint32(json::unsignedField(at(d, "threshold"), "threshold", 0xffffffffu));
    static const std::pair<const char *, HashSize> sizes[] = {{"8x8", HashSize::S8x8}, {"16x16", HashSize::S16x16}, {"32x32", HashSize::S32x32}};
    static const std::pair<const char *, HashAlgorithm> algs[] = {{"blockhash", HashAlgorithm::Blockhash}, {"phash", HashAlgorithm::Phash}, {"dhash", HashAlgorithm::Dhash}};
    static const std::pair<const char *, ResizeFilter> filters[] = {{"nearest", ResizeFilter::Nearest}, {"triangle", ResizeFilter::Triangle}, {"catmullrom", ResizeFilter::Catmullrom}, {"lanczos3", ResizeFilter::Lanczos3}};
    s.duplicates.hashSize = parseEnum(at(d, "hashSize"), "hashSize", sizes);
    s.duplicates.hashAlg = parseEnum(at(d, "hashAlg"), "hashAlg", algs);
    s.duplicates.resizeFilter = parseEnum(at(d, "resizeFilter"), "resizeFilter", filters);
    s.duplicates.useThumbnailsFirst = json::boolField(at(d, "useThumbnailsFirst"), "useThumbnailsFirst");
    s.duplicates.maxFilesPerAlbum = quint32(json::unsignedField(at(d, "maxFilesPerAlbum"), "maxFilesPerAlbum", 0xffffffffu));
  }
  QJsonObject t = section(o, "thumbnails", present);
  if (present) {
    s.thumbnails.maxDim = quint32(json::unsignedField(at(t, "maxDim"), "maxDim", 0xffffffffu));
    s.thumbnails.imageWebpQuality = quint32(json::unsignedField(at(t, "imageWebpQuality"), "imageWebpQuality", 255));
    s.thumbnails.imageWebpCompressionLevel = quint32(json::unsignedField(at(t, "imageWebpCompressionLevel"), "imageWebpCompressionLevel", 255));
    s.thumbnails.videoSeekSeconds = float(json::floatField(at(t, "videoSeekSeconds"), "videoSeekSeconds"));
    s.thumbnails.lockPollMs = json::unsignedField(at(t, "lockPollMs"), "lockPollMs", ~0ull);
  }
  QJsonObject f = section(o, "ffmpeg", present);
  if (present) {
    const QJsonValue threads = at(f, "threads");
    if (threads.isString()) {
      const QString text = threads.toString();
      bool ok = false;
      const uint n = text.toUInt(&ok);
      if (text.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0 || !ok || n > 255) s.ffmpeg.threads = {true, 4};
      else s.ffmpeg.threads = {false, int(n)};
    } else {
      s.ffmpeg.threads = {false, int(json::unsignedField(threads, "threads", 255))};
    }
    s.ffmpeg.timeoutSecs = json::unsignedField(at(f, "timeoutSecs"), "timeoutSecs", ~0ull);
    s.ffmpeg.hwaccel = json::stringField(at(f, "hwaccel"), "hwaccel");
    s.ffmpeg.processWaitPollMs = json::unsignedField(at(f, "processWaitPollMs"), "processWaitPollMs", ~0ull);
  }
  QJsonObject p = section(o, "preload", present);
  if (present) {
    s.preload.thumbWorkers = quint32(json::unsignedField(at(p, "thumbWorkers"), "thumbWorkers", 255));
    s.preload.metaWorkers = quint32(json::unsignedField(at(p, "metaWorkers"), "metaWorkers", 255));
    s.preload.hashWorkers = quint32(json::unsignedField(at(p, "hashWorkers"), "hashWorkers", 255));
    // Settings files of earlier releases do not have it.
    if (const QJsonValue encrypt = p.value(QLatin1String("encryptWorkers")); !encrypt.isUndefined()) s.preload.encryptWorkers = quint32(json::unsignedField(encrypt, "encryptWorkers", 255));
    s.preload.progressEmitMs = json::unsignedField(at(p, "progressEmitMs"), "progressEmitMs", ~0ull);
    s.preload.thumbHashQueueDelayMs = json::unsignedField(at(p, "thumbHashQueueDelayMs"), "thumbHashQueueDelayMs", ~0ull);
    s.preload.thumbHashOnlyAfterIdle = json::boolField(at(p, "thumbHashOnlyAfterIdle"), "thumbHashOnlyAfterIdle");
    s.preload.thumbHashRetryOnThumbChange = json::boolField(at(p, "thumbHashRetryOnThumbChange"), "thumbHashRetryOnThumbChange");
  }
  QJsonObject m = section(o, "metadata", present);
  if (present) {
    const QJsonValue timeout = at(m, "ffmpegProbeTimeoutSecs");
    if (!timeout.isUndefined() && !timeout.isNull()) s.metadata.ffmpegProbeTimeoutSecs = json::unsignedField(timeout, "ffmpegProbeTimeoutSecs", ~0ull);
    s.metadata.parseCreationTime = json::boolField(at(m, "parseCreationTime"), "parseCreationTime");
  }
  QJsonObject a = section(o, "album", present);
  if (present) {
    s.album.renameCleanupDelaySecs = json::unsignedField(at(a, "renameCleanupDelaySecs"), "renameCleanupDelaySecs", ~0ull);
    s.album.moveRenameThumbsAndMeta = json::boolField(at(a, "moveRenameThumbsAndMeta"), "moveRenameThumbsAndMeta");
  }
  QJsonObject pr = section(o, "privacy", present);
  if (present) {
    s.privacy.enabled = json::boolField(at(pr, "enabled"), "enabled");
    s.privacy.lockscreenEnabled = json::boolField(at(pr, "lockscreenEnabled"), "lockscreenEnabled");
    s.privacy.confirmOpenEnabled = json::boolField(at(pr, "confirmOpenEnabled"), "confirmOpenEnabled");
    // Settings files of earlier releases do not have it.
    const QJsonValue hardening = pr.value(QLatin1String("hardening"));
    if (!hardening.isUndefined()) s.privacy.hardening = json::boolField(hardening, "hardening");
  }
  return s;
}

QString AdvancedSettings::toJsonText() const {
  json::Object privacyObject;
  privacyObject.flag("enabled", privacy.enabled).flag("lockscreenEnabled", privacy.lockscreenEnabled).flag("confirmOpenEnabled", privacy.confirmOpenEnabled).flag("hardening", privacy.hardening);
  json::Object albumObject;
  albumObject.unum("renameCleanupDelaySecs", album.renameCleanupDelaySecs).flag("moveRenameThumbsAndMeta", album.moveRenameThumbsAndMeta);
  json::Object preloadObject;
  preloadObject.unum("thumbWorkers", preload.thumbWorkers).unum("metaWorkers", preload.metaWorkers).unum("hashWorkers", preload.hashWorkers).unum("progressEmitMs", preload.progressEmitMs).unum("thumbHashQueueDelayMs", preload.thumbHashQueueDelayMs).flag("thumbHashOnlyAfterIdle", preload.thumbHashOnlyAfterIdle).flag("thumbHashRetryOnThumbChange", preload.thumbHashRetryOnThumbChange).unum("encryptWorkers", preload.encryptWorkers);
  json::Object all;
  all.raw("duplicates", duplicatesJson(duplicates).compact()).raw("thumbnails", thumbnailsJson(thumbnails).compact()).raw("ffmpeg", ffmpegJson(ffmpeg).compact()).raw("preload", preloadObject.compact()).raw("metadata", metadataJson(metadata).compact()).raw("album", albumObject.compact()).raw("privacy", privacyObject.compact());
  return all.pretty();
}

QJsonObject AdvancedSettings::toJson() const { return QJsonDocument::fromJson(toJsonText().toUtf8()).object(); }

AdvancedSettings AdvancedSettings::clamp() const {
  AdvancedSettings s = *this;
  s.duplicates.threshold = std::min(s.duplicates.threshold, 128u);
  s.duplicates.maxFilesPerAlbum = std::min(s.duplicates.maxFilesPerAlbum, 20000u);
  s.thumbnails.maxDim = std::clamp(s.thumbnails.maxDim, 128u, 2048u);
  s.thumbnails.imageWebpQuality = std::clamp(s.thumbnails.imageWebpQuality, 30u, 95u);
  s.thumbnails.imageWebpCompressionLevel = std::clamp(s.thumbnails.imageWebpCompressionLevel, 0u, 6u);
  s.thumbnails.videoSeekSeconds = std::isnan(s.thumbnails.videoSeekSeconds) ? 0.0f : std::clamp(s.thumbnails.videoSeekSeconds, 0.0f, 30.0f);
  s.thumbnails.lockPollMs = std::clamp<quint64>(s.thumbnails.lockPollMs, 5, 250);
  if (!s.ffmpeg.threads.automatic) s.ffmpeg.threads.count = std::clamp(s.ffmpeg.threads.count, 1, 32);
  s.ffmpeg.timeoutSecs = std::clamp<quint64>(s.ffmpeg.timeoutSecs, 1, 60);
  if (s.ffmpeg.hwaccel.trimmed().isEmpty()) s.ffmpeg.hwaccel = QStringLiteral("auto");
  s.ffmpeg.processWaitPollMs = std::clamp<quint64>(s.ffmpeg.processWaitPollMs, 5, 200);
  s.preload.thumbWorkers = std::clamp(s.preload.thumbWorkers, 1u, 32u);
  s.preload.metaWorkers = std::clamp(s.preload.metaWorkers, 1u, 32u);
  s.preload.hashWorkers = std::clamp(s.preload.hashWorkers, 1u, 32u);
  s.preload.encryptWorkers = std::clamp(s.preload.encryptWorkers, 1u, 32u);
  s.preload.progressEmitMs = std::clamp<quint64>(s.preload.progressEmitMs, 50, 1000);
  s.preload.thumbHashQueueDelayMs = std::clamp<quint64>(s.preload.thumbHashQueueDelayMs, 0, 100);
  s.metadata.ffmpegProbeTimeoutSecs = s.metadata.ffmpegProbeTimeoutSecs ? std::clamp<quint64>(*s.metadata.ffmpegProbeTimeoutSecs, 1, 60) : s.ffmpeg.timeoutSecs;
  s.album.renameCleanupDelaySecs = std::clamp<quint64>(s.album.renameCleanupDelaySecs, 0, 10);
  if (s.privacy.enabled) s.privacy.lockscreenEnabled = s.privacy.confirmOpenEnabled = true;
  if (s.privacy.hardening) s.privacy.lockscreenEnabled = true;
  return s;
}

QString thumbnailPipeline() {
#ifdef Q_OS_MACOS
  static const bool native = qEnvironmentVariable("ROOM237_NATIVE_THUMB") != QLatin1String("0");
  if (native) return QStringLiteral("imageio-libwebp-1");
#endif
  return QStringLiteral("ffmpeg");
}
QString hashDecoder() {
#ifdef Q_OS_MACOS
  static const bool native = qEnvironmentVariable("ROOM237_NATIVE_HASH") != QLatin1String("0");
  if (native) return QStringLiteral("libwebp");
#endif
  return QStringLiteral("image");
}

QString AdvancedSettings::thumbVersion() const { return thumbVersionFor(thumbnailPipeline()); }
QString AdvancedSettings::thumbVersionFor(const QString &pipeline) const {
  const QString text = QStringLiteral("dim:%1|q:%2|c:%3|seek:%4|pipeline:%5").arg(thumbnails.maxDim).arg(thumbnails.imageWebpQuality).arg(thumbnails.imageWebpCompressionLevel).arg(QString::asprintf("%.3f", double(thumbnails.videoSeekSeconds))).arg(pipeline);
  return sha256Hex(text.toUtf8()).left(16);
}
QString AdvancedSettings::metadataVersion() const {
  return QStringLiteral("signed-date-v2|display-size-v1|creation-time:%1").arg(json::boolean(metadata.parseCreationTime));
}
QString AdvancedSettings::processingVersion() const {
  const QString encoded = QStringLiteral("[%1,%2,%3,%4]").arg(thumbnailsJson(thumbnails).compact(), ffmpegJson(ffmpeg).compact(), metadataJson(metadata).compact(), json::string(hashConfig().hashVersion));
  QCryptographicHash hasher(QCryptographicHash::Sha256);
  hasher.addData(QByteArrayLiteral("blur-webp-16-v1|"));
  hasher.addData(encoded.toUtf8());
  return toHex(hasher.result()).left(16);
}
HashConfig AdvancedSettings::hashConfig() const { return hashConfigFor(thumbVersion()); }
HashConfig AdvancedSettings::hashConfigFor(const QString &thumbVersionValue) const {
  HashConfig config;
  config.width = config.height = hashSide(duplicates.hashSize);
  config.bits = quint32(config.width * config.height);
  config.alg = duplicates.hashAlg;
  config.resizeFilter = duplicates.resizeFilter;
  config.useThumbnailsFirst = duplicates.useThumbnailsFirst;
  config.thumbVersion = duplicates.useThumbnailsFirst ? thumbVersionValue : QString();
  QCryptographicHash hasher(QCryptographicHash::Sha256);
  hasher.addData(QByteArray(kHashSchemaVersion));
  hasher.addData(QStringLiteral("alg:%1|size:%2x%3|filter:%4|thumb_first:%5|thumb_version:%6|decoder:%7").arg(QLatin1String(algDebug(duplicates.hashAlg))).arg(config.width).arg(config.height).arg(QLatin1String(filterDebug(duplicates.resizeFilter))).arg(json::boolean(duplicates.useThumbnailsFirst)).arg(config.thumbVersion).arg(hashDecoder()).toUtf8());
  config.hashVersion = toHex(hasher.result());
  config.userThreshold = duplicates.threshold;
  config.effectiveThreshold = duplicates.effectiveThreshold();
  config.thumbSettings = thumbnails;
  return config;
}

AdvancedSettings defaultSettings() { return AdvancedSettings().clamp(); }

AdvancedSettings loadSettingsFile(const QString &path) {
  QFile file(path);
  if (!file.exists()) return defaultSettings();
  if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read settings %1: %2").arg(path, file.errorString()));
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) fail(QStringLiteral("Invalid settings in %1: %2").arg(path, error.error != QJsonParseError::NoError ? error.errorString() : QStringLiteral("expected an object")));
  try {
    return AdvancedSettings::fromJson(document.object()).clamp();
  } catch (const Error &e) {
    fail(QStringLiteral("Invalid settings in %1: %2").arg(path, e.message()));
  }
}

AdvancedSettings readSettings() {
  std::shared_lock lock(settingsMutex);
  return current ? *current : AdvancedSettings();
}

void SettingsStore::load(const QString &profile) {
  const QString file = path::join(profile, QStringLiteral("settings.json"));
  AdvancedSettings loaded = loadSettingsFile(file);
  std::unique_lock lock(settingsMutex);
  settingsPath = file;
  current = loaded;
}
AdvancedSettings SettingsStore::get() {
  std::shared_lock lock(settingsMutex);
  return current ? *current : defaultSettings();
}
AdvancedSettings SettingsStore::update(const AdvancedSettings &next) {
  const AdvancedSettings validated = next.clamp();
  {
    std::unique_lock lock(settingsMutex);
    atomicWrite(settingsPath, validated.toJsonText().toUtf8());
    current = validated;
  }
  preload::applyWorkerLimits();
  return validated;
}
AdvancedSettings SettingsStore::reset() { return update(AdvancedSettings()); }

}  // namespace core
