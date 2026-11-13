#pragma once
#include <QJsonObject>
#include <QString>
#include <optional>

namespace core {

enum class HashSize { S8x8, S16x16, S32x32 };
enum class HashAlgorithm { Blockhash, Phash, Dhash };
enum class ResizeFilter { Nearest, Triangle, Catmullrom, Lanczos3 };
int hashSide(HashSize size);

struct ThreadSetting {
  bool automatic = false;
  int count = 4;
  int resolved() const;
};

struct DuplicatesSettings {
  quint32 threshold = 32;
  HashSize hashSize = HashSize::S16x16;
  HashAlgorithm hashAlg = HashAlgorithm::Blockhash;
  ResizeFilter resizeFilter = ResizeFilter::Nearest;
  bool useThumbnailsFirst = true;
  quint32 maxFilesPerAlbum = 0;  // No effect since 7.0; kept so that older settings files round-trip.
  quint32 effectiveThreshold() const;
};
struct ThumbnailSettings {
  quint32 maxDim = 450;
  quint32 imageWebpQuality = 75;
  quint32 imageWebpCompressionLevel = 2;
  float videoSeekSeconds = 1.0f;
  quint64 lockPollMs = 50;
};
struct FfmpegSettings {
  ThreadSetting threads;
  quint64 timeoutSecs = 5;
  QString hwaccel = QStringLiteral("auto");
  quint64 processWaitPollMs = 50;
  int resolvedThreads() const { return threads.resolved(); }
};
struct PreloadSettings {
  quint32 thumbWorkers = 4, metaWorkers = 4, hashWorkers = 8;
  quint32 encryptWorkers = 4;  // Files encrypted or decrypted at once by security hardening.
  quint64 progressEmitMs = 100, thumbHashQueueDelayMs = 10;
  bool thumbHashOnlyAfterIdle = true;  // No effect; kept so that older settings files round-trip.
  bool thumbHashRetryOnThumbChange = true;
};
struct MetadataSettings {
  std::optional<quint64> ffmpegProbeTimeoutSecs;
  bool parseCreationTime = true;
};
struct AlbumSettings {
  quint64 renameCleanupDelaySecs = 1;
  bool moveRenameThumbsAndMeta = true;
};
struct PrivacySettings {
  bool enabled = false, lockscreenEnabled = false, confirmOpenEnabled = false;
  bool hardening = false;  // Security hardening: encrypted roots; implies the lock screen.
};

struct HashConfig {
  int width = 16, height = 16;
  quint32 bits = 256;
  HashAlgorithm alg = HashAlgorithm::Blockhash;
  ResizeFilter resizeFilter = ResizeFilter::Nearest;
  bool useThumbnailsFirst = true;
  QString thumbVersion, hashVersion;
  quint32 userThreshold = 32, effectiveThreshold = 32;
  ThumbnailSettings thumbSettings;
};

struct AdvancedSettings {
  DuplicatesSettings duplicates;
  ThumbnailSettings thumbnails;
  FfmpegSettings ffmpeg;
  PreloadSettings preload;
  MetadataSettings metadata;
  AlbumSettings album;
  PrivacySettings privacy;

  static AdvancedSettings fromJson(const QJsonObject &object);  // Throws on a value serde would refuse.
  QJsonObject toJson() const;
  QString toJsonText() const;  // Pretty, field order of the earlier releases.
  AdvancedSettings clamp() const;
  QString thumbVersion() const;
  QString thumbVersionFor(const QString &pipeline) const;
  QString metadataVersion() const;
  QString processingVersion() const;
  HashConfig hashConfig() const;
  HashConfig hashConfigFor(const QString &thumbVersion) const;
};

AdvancedSettings defaultSettings();  // Clamped, as a missing file loads.
AdvancedSettings readSettings();
AdvancedSettings loadSettingsFile(const QString &path);

// The profile's settings.json. load() refuses corrupt settings: defaults could silently turn privacy off.
class SettingsStore {
public:
  static void load(const QString &profile);
  static AdvancedSettings get();
  static AdvancedSettings update(const AdvancedSettings &next);
  static AdvancedSettings reset();
};

QString thumbnailPipeline();
QString hashDecoder();

}  // namespace core
