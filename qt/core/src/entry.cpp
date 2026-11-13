#include "entry.h"

#include "fs.h"
#include "json.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace core {

namespace {
using u128 = unsigned __int128;

std::optional<QString> optString(const QJsonObject &o, const char *key) {
  const QJsonValue v = o.value(QLatin1String(key));
  if (v.isUndefined() || v.isNull()) return std::nullopt;
  return json::stringField(v, key);
}
bool flag(const QJsonObject &o, const char *key) {
  const QJsonValue v = o.value(QLatin1String(key));
  if (v.isUndefined()) return false;
  return json::boolField(v, key);
}
QString optJson(const std::optional<QString> &value) { return value ? json::string(*value) : QStringLiteral("null"); }

QString u128ToString(u128 value) {
  if (value == 0) return QStringLiteral("0");
  char digits[48];
  int n = 0;
  while (value) {
    digits[n++] = char('0' + int(value % 10));
    value /= 10;
  }
  QString out;
  out.reserve(n);
  while (n) out += QLatin1Char(digits[--n]);
  return out;
}
std::optional<u128> parseU128(const QString &text, QString *error) {
  QString digits = text;
  if (digits.startsWith(QLatin1Char('+'))) digits.remove(0, 1);
  if (digits.isEmpty()) {
    *error = QStringLiteral("cannot parse integer from empty string");
    return std::nullopt;
  }
  u128 value = 0;
  const u128 max = ~u128(0);
  for (QChar c : digits) {
    if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
      *error = QStringLiteral("invalid digit found in string");
      return std::nullopt;
    }
    const unsigned d = unsigned(c.unicode() - '0');
    if (value > (max - d) / 10) {
      *error = QStringLiteral("number too large to fit in target type");
      return std::nullopt;
    }
    value = value * 10 + d;
  }
  return value;
}
}  // namespace

QString FileMetaEntry::toJson() const {
  json::Object o;
  if (blurPreview) o.str("blur_preview", *blurPreview);
  if (heicConversion) o.raw("heic_conversion", json::Object().str("output_name", heicConversion->outputName).str("source_revision", heicConversion->sourceRevision).str("output_revision", heicConversion->outputRevision).compact());
  o.raw("meta", optJson(meta));
  if (metaSourceRevision) o.str("meta_source_revision", *metaSourceRevision);
  if (metaEmbeddedTimestamp) o.num("meta_embedded_timestamp", *metaEmbeddedTimestamp);
  if (metaEmbeddedRevision) o.str("meta_embedded_revision", *metaEmbeddedRevision);
  if (metaSettingsVersion) o.str("meta_settings_version", *metaSettingsVersion);
  if (thumbFailedProcessingVersion) o.str("thumb_failed_processing_version", *thumbFailedProcessingVersion);
  if (metaFailedProcessingVersion) o.str("meta_failed_processing_version", *metaFailedProcessingVersion);
  if (hashFailedProcessingVersion) o.str("hash_failed_processing_version", *hashFailedProcessingVersion);
  if (metaFailedSourceRevision) o.str("meta_failed_source_revision", *metaFailedSourceRevision);
  if (thumbFailedSourceRevision) o.str("thumb_failed_source_revision", *thumbFailedSourceRevision);
  o.flag("favorite", favorite);
  o.raw("hash", optJson(hash));
  o.raw("hash_version", optJson(hashVersion));
  o.raw("hash_bits", hashBits ? QString::number(*hashBits) : QStringLiteral("null"));
  if (hashSourceRevision) o.str("hash_source_revision", *hashSourceRevision);
  if (hashFailedSourceRevision) o.str("hash_failed_source_revision", *hashFailedSourceRevision);
  if (thumbSourceRevision) o.str("thumb_source_revision", *thumbSourceRevision);
  if (thumbSourceHash) o.str("thumb_source_hash", *thumbSourceHash);
  if (thumbHashRevision) o.str("thumb_hash_revision", *thumbHashRevision);
  o.raw("thumb_version", optJson(thumbVersion));
  o.flag("hash_failed", hashFailed).flag("thumb_failed", thumbFailed).flag("meta_failed", metaFailed);
  if (vaultName) o.str("vault_name", *vaultName);
  return o.compact();
}

QJsonObject FileMetaEntry::toJsonObject() const { return QJsonDocument::fromJson(toJson().toUtf8()).object(); }

FileMetaEntry FileMetaEntry::fromJsonObject(const QJsonObject &o) {
  FileMetaEntry e;
  e.blurPreview = optString(o, "blur_preview");
  const QJsonValue heic = o.value(QLatin1String("heic_conversion"));
  if (!heic.isUndefined() && !heic.isNull()) {
    if (!heic.isObject()) fail(QStringLiteral("heic_conversion: invalid type, expected struct HeicConversion"));
    const QJsonObject h = heic.toObject();
    e.heicConversion = HeicConversion{json::stringField(h.value(QLatin1String("output_name")), "output_name"), json::stringField(h.value(QLatin1String("source_revision")), "source_revision"), json::stringField(h.value(QLatin1String("output_revision")), "output_revision")};
  }
  e.meta = optString(o, "meta");
  e.metaSourceRevision = optString(o, "meta_source_revision");
  const QJsonValue ts = o.value(QLatin1String("meta_embedded_timestamp"));
  if (!ts.isUndefined() && !ts.isNull()) e.metaEmbeddedTimestamp = json::signedField(ts, "meta_embedded_timestamp");
  e.metaEmbeddedRevision = optString(o, "meta_embedded_revision");
  e.metaSettingsVersion = optString(o, "meta_settings_version");
  e.thumbFailedProcessingVersion = optString(o, "thumb_failed_processing_version");
  e.metaFailedProcessingVersion = optString(o, "meta_failed_processing_version");
  e.hashFailedProcessingVersion = optString(o, "hash_failed_processing_version");
  e.metaFailedSourceRevision = optString(o, "meta_failed_source_revision");
  e.thumbFailedSourceRevision = optString(o, "thumb_failed_source_revision");
  e.favorite = flag(o, "favorite");
  e.hash = optString(o, "hash");
  e.hashVersion = optString(o, "hash_version");
  const QJsonValue bits = o.value(QLatin1String("hash_bits"));
  if (!bits.isUndefined() && !bits.isNull()) e.hashBits = quint32(json::unsignedField(bits, "hash_bits", 0xffffffffu));
  e.hashSourceRevision = optString(o, "hash_source_revision");
  e.hashFailedSourceRevision = optString(o, "hash_failed_source_revision");
  e.thumbSourceRevision = optString(o, "thumb_source_revision");
  e.thumbSourceHash = optString(o, "thumb_source_hash");
  e.thumbHashRevision = optString(o, "thumb_hash_revision");
  e.thumbVersion = optString(o, "thumb_version");
  e.hashFailed = flag(o, "hash_failed");
  e.thumbFailed = flag(o, "thumb_failed");
  e.metaFailed = flag(o, "meta_failed");
  e.vaultName = optString(o, "vault_name");
  return e;
}

FileMetaEntry FileMetaEntry::fromJson(const QString &text) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError) fail(error.errorString());
  if (!document.isObject()) fail(QStringLiteral("invalid type: expected struct FileMetaEntry"));
  return fromJsonObject(document.object());
}

QString DetachedFileMeta::pack() const {
  u128 packed = 0;
  const u128 mask40 = (u128(1) << 40) - 1, mask20 = (u128(1) << 20) - 1;
  packed |= u128(a.value_or(0)) & mask40;
  packed |= (u128(quint64(s.value_or(0))) & mask40) << 40;
  packed |= (u128(w.value_or(0)) & mask20) << 80;
  packed |= (u128(h.value_or(0)) & mask20) << 100;
  if (i) packed |= u128(1) << 120;
  if (v) packed |= u128(1) << 121;
  if (a) packed |= u128(1) << 122;
  if (s) packed |= u128(1) << 123;
  if (w) packed |= u128(1) << 124;
  if (h) packed |= u128(1) << 125;
  // Bit 126 marks negative two's-complement dates; the unsigned 40-bit range stays complete.
  if (s && *s < 0) packed |= u128(1) << 126;
  return u128ToString(packed);
}

DetachedFileMeta unpackFileMeta(const QString &packed) {
  QString error;
  const auto parsed = parseU128(packed, &error);
  if (!parsed) fail(QStringLiteral("Invalid packed meta: %1").arg(error));
  const u128 p = *parsed;
  const u128 mask40 = (u128(1) << 40) - 1, mask20 = (u128(1) << 20) - 1;
  const quint64 added = quint64(p & mask40);
  qint64 shoot = qint64(quint64((p >> 40) & mask40));
  if (p & (u128(1) << 126)) shoot -= qint64(1) << 40;
  DetachedFileMeta meta;
  if (p & (u128(1) << 122)) meta.a = added;
  if (p & (u128(1) << 123)) meta.s = shoot;
  meta.i = p & (u128(1) << 120);
  meta.v = p & (u128(1) << 121);
  if (p & (u128(1) << 124)) meta.w = quint32((p >> 80) & mask20);
  if (p & (u128(1) << 125)) meta.h = quint32((p >> 100) & mask20);
  return meta;
}

bool validPackedMeta(const QString &packed) {
  QString error;
  return parseU128(packed, &error).has_value();
}

QJsonObject DetachedMediaEntry::toJson() const {
  QJsonObject o;
  if (blurPreview) o.insert(QStringLiteral("blurPreview"), *blurPreview);
  o.insert(QStringLiteral("revision"), revision);
  o.insert(QStringLiteral("meta"), meta);
  o.insert(QStringLiteral("name"), name);
  if (thumbnailName) o.insert(QStringLiteral("thumbnailName"), *thumbnailName);
  if (favorite) o.insert(QStringLiteral("favorite"), *favorite);
  if (displayName) o.insert(QStringLiteral("displayName"), *displayName);
  return o;
}

QJsonObject DetachedAlbum::toJson() const {
  QJsonObject o;
  o.insert(QStringLiteral("path"), path);
  o.insert(QStringLiteral("name"), name);
  o.insert(QStringLiteral("size"), size);
  o.insert(QStringLiteral("thumb_path"), thumbPath ? QJsonValue(*thumbPath) : QJsonValue());
  o.insert(QStringLiteral("relative_path"), relativePath);
  if (parent) o.insert(QStringLiteral("parent"), *parent);
  o.insert(QStringLiteral("directory_id"), directoryId ? QJsonValue(*directoryId) : QJsonValue());
  return o;
}

}  // namespace core
