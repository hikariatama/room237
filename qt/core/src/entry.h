#pragma once
// Per-file metadata records of the album database and their JSON form.
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <optional>
#include <vector>

namespace core {

struct HeicConversion {
  QString outputName, sourceRevision, outputRevision;
  bool operator==(const HeicConversion &) const = default;
};

struct FileMetaEntry {
  std::optional<QString> blurPreview;
  std::optional<HeicConversion> heicConversion;
  std::optional<QString> meta;
  std::optional<QString> metaSourceRevision;
  std::optional<qint64> metaEmbeddedTimestamp;
  std::optional<QString> metaEmbeddedRevision;
  std::optional<QString> metaSettingsVersion;
  std::optional<QString> thumbFailedProcessingVersion;
  std::optional<QString> metaFailedProcessingVersion;
  std::optional<QString> hashFailedProcessingVersion;
  std::optional<QString> metaFailedSourceRevision;
  std::optional<QString> thumbFailedSourceRevision;
  bool favorite = false;
  std::optional<QString> hash;
  std::optional<QString> hashVersion;
  std::optional<quint32> hashBits;
  std::optional<QString> hashSourceRevision;
  std::optional<QString> hashFailedSourceRevision;
  std::optional<QString> thumbSourceRevision;
  std::optional<QString> thumbSourceHash;
  std::optional<QString> thumbHashRevision;
  std::optional<QString> thumbVersion;
  bool hashFailed = false, thumbFailed = false, metaFailed = false;
  std::optional<QString> vaultName;  // The original name of an encrypted file; stored only in sealed rows.

  bool operator==(const FileMetaEntry &) const = default;
  QString toJson() const;  // serde_json's compact text, as stored by earlier releases.
  QJsonObject toJsonObject() const;
  static FileMetaEntry fromJson(const QString &text);
  static FileMetaEntry fromJsonObject(const QJsonObject &object);
};

struct AlbumMeta {
  QHash<QString, FileMetaEntry> files;
  std::vector<std::pair<QString, QString>> duplicatesIgnore;
};

// The packed 128-bit summary the interface decodes: added/shot times, kind and display size.
struct DetachedFileMeta {
  std::optional<quint64> a;
  std::optional<qint64> s;
  bool i = false, v = false;
  std::optional<quint32> w, h;
  QString pack() const;
};
DetachedFileMeta unpackFileMeta(const QString &packed);  // Throws "Invalid packed meta: ...".
bool validPackedMeta(const QString &packed);

struct DetachedMediaEntry {
  std::optional<QString> blurPreview;
  QString revision, meta, name;
  std::optional<QString> thumbnailName;
  std::optional<bool> favorite;
  std::optional<QString> displayName;  // The original name when the stored one is the random name of an encrypted file.
  QJsonObject toJson() const;
};

struct DetachedAlbum {
  QString path, name;
  qint64 size = 0;
  std::optional<QString> thumbPath;
  QString relativePath;
  std::optional<QString> parent;
  std::optional<QString> directoryId;
  QJsonObject toJson() const;
};

}  // namespace core
