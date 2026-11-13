#include "update.h"

#include "fs.h"
#include "json.h"
#include "util.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUrl>
#include <QRegularExpression>
#include <thread>
#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <zlib.h>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace core::update {

namespace {
#ifndef ROOM237_VERSION
#define ROOM237_VERSION "0.0.0"
#endif
#ifdef ROOM237_UPDATE_PUBKEY
const char *const kPublicKey = ROOM237_UPDATE_PUBKEY;
#else
// The existing Tauri updater key.
const char *const kPublicKey = "RWSptBXbOJ4mm7I37d8OEZ6Qs+I/q1PZQP+J6d75ZUEma6Kfmr732npU";
#endif
const char *const kAllowedHosts[] = {"github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"};
constexpr quint64 kMaxPayload = quint64(2) << 30;
constexpr quint64 kMaxUnpacked = quint64(8) << 30;

QString sibling(const QString &install, const QString &suffix) {
  const QString name = path::fileName(install);
  if (name.isEmpty()) fail(QStringLiteral("Install path has no name"));
  return path::join(path::parent(install), QStringLiteral(".%1.%2").arg(name, suffix));
}

void removePath(const QString &p) {
  const FileInfo info = lstat(p);
  if (!info.exists) return;
  if (info.isDir) removeTree(p);
  else removeFile(p);
}

// --- QA fixture (QA builds only) ---------------------------------------------------------------------------------

struct QaFixture {
  QString feed, staging, install, executable, root, profile, legacy, home, publicKey;
};

QString canonicalFs(const QString &p) {
  QString real = canonical(p);
#ifdef Q_OS_WIN
  // canonical() uses extended-length paths; Qt and argv use ordinary drive paths.
  if (real.startsWith(QLatin1String("\\\\?\\"))) real = real.mid(4);
#endif
  return real;
}
QString slashes(QString p) { return p.replace(QLatin1Char('\\'), QLatin1Char('/')); }

QString canonicalTaskPath(const QString &raw, const QString &fence) {
  if (!path::isAbsolute(raw)) fail(QStringLiteral("QA update requires canonical absolute paths"));
  for (const QString &part : slashes(raw).split(QLatin1Char('/')))
    if (part == QLatin1String(".") || part == QLatin1String("..")) fail(QStringLiteral("QA update requires canonical absolute paths"));
  QString ancestor = raw;
  QStringList missing;
  while (!exists(ancestor)) {
    if (lstat(ancestor).exists) fail(QStringLiteral("QA dangling symlink"));
    const QString name = path::fileName(ancestor);
    if (name.isEmpty()) fail(QStringLiteral("QA path has no ancestor"));
    missing << name;
    ancestor = path::parent(ancestor);
    if (ancestor.isEmpty()) fail(QStringLiteral("QA path has no parent"));
  }
  if (!missing.isEmpty() && !isDir(ancestor)) fail(QStringLiteral("QA invalid ancestor"));
  QString real = canonicalFs(ancestor);
  for (auto it = missing.crbegin(); it != missing.crend(); ++it) real = path::join(real, *it);
  if (slashes(real) != slashes(raw) || !path::startsWith(real, fence)) fail(QStringLiteral("QA update path outside canonical fence"));
  return real;
}

std::optional<QaFixture> qaFixture();

void qaUrl(const QString &raw, const QaFixture &f) {
  const QUrl url(raw, QUrl::StrictMode);
  if (!url.isValid()) fail(url.errorString());
  if (!url.userName().isEmpty() || !url.password().isEmpty() || url.hasQuery() || url.hasFragment()) fail(QStringLiteral("QA update URL credentials/query refused"));
  if (url.scheme() == QLatin1String("file") && url.host().isEmpty()) {
    canonicalTaskPath(QDir::toNativeSeparators(url.toLocalFile()), qEnvironmentVariable("ROOM237_QA_FENCE"));
    return;
  }
  const QUrl feed(f.feed);
  if (url.scheme() == QLatin1String("http") && url.host() == QLatin1String("127.0.0.1") && url.port() != -1 && url.port() == feed.port()) return;
  fail(QStringLiteral("QA update URL is not the allowlisted local feed"));
}

std::optional<QaFixture> qaFixture() {
  if (!qaRun()) return std::nullopt;
  if (!qEnvironmentVariableIsSet("ROOM237_QA_UPDATE_FIXTURE")) fail(QStringLiteral("QA updater disabled"));
  const QJsonDocument document = QJsonDocument::fromJson(qEnvironmentVariable("ROOM237_QA_UPDATE_FIXTURE").toUtf8());
  if (!document.isObject()) fail(QStringLiteral("Invalid QA update fixture"));
  const QJsonObject o = document.object();
  QaFixture f;
  f.feed = json::stringField(o.value(QLatin1String("feed")), "feed");
  f.staging = json::stringField(o.value(QLatin1String("staging")), "staging");
  f.install = json::stringField(o.value(QLatin1String("install")), "install");
  f.executable = json::stringField(o.value(QLatin1String("executable")), "executable");
  f.root = json::stringField(o.value(QLatin1String("root")), "root");
  f.profile = json::stringField(o.value(QLatin1String("profile")), "profile");
  f.legacy = json::stringField(o.value(QLatin1String("legacy")), "legacy");
  f.home = json::stringField(o.value(QLatin1String("home")), "home");
  f.publicKey = json::stringField(o.value(QLatin1String("publicKey")), "publicKey");
  if (!qEnvironmentVariableIsSet("ROOM237_QA_FENCE")) fail(QStringLiteral("QA update fence missing"));
  const QString raw = qEnvironmentVariable("ROOM237_QA_FENCE");
  const QString fence = canonicalFs(raw);
  if (!path::isAbsolute(raw) || fence != raw || path::parent(fence).isEmpty()) fail(QStringLiteral("QA invalid update fence"));
  canonicalTaskPath(raw, fence);
  for (const QString *p : {&f.staging, &f.install, &f.executable, &f.root, &f.profile, &f.legacy, &f.home}) canonicalTaskPath(*p, fence);
  for (const char *suffix : {"update-new", "update-old", "update-failed"}) canonicalTaskPath(sibling(f.install, QString::fromLatin1(suffix)), fence);
  for (const char *leaf : {"updates", "update-plan.json", "update-health", "update-result.json", "room237-update-helper", "room237-update-helper.exe"}) canonicalTaskPath(path::join(f.profile, QString::fromLatin1(leaf)), fence);
  if (!path::startsWith(f.executable, f.install) || !isFile(f.executable) || !isDir(f.root) || !isDir(f.profile) || !isDir(f.legacy) || qEnvironmentVariable("HOME") != f.home || sibling(f.install, QStringLiteral("update-new")) != f.staging)
    fail(QStringLiteral("QA update fixture mismatch"));
  // A direct core test must also run from its explicit task namespace.
  canonicalTaskPath(QDir::currentPath(), fence);
  qaUrl(f.feed, f);
  verifySignature(f.publicKey, {}, {});  // Throws for a malformed key before the signature itself is parsed.
  return f;
}

// --- minisign ----------------------------------------------------------------------------------------------------

struct PublicKey {
  QByteArray keyId, key;
};
PublicKey parsePublicKey(const QString &base64) {
  const auto decoded = fromBase64(base64.trimmed().toLatin1());
  if (!decoded || decoded->size() != 42 || !decoded->startsWith("Ed")) fail(QStringLiteral("update key: Invalid encoding in minisign data"));
  return {decoded->mid(2, 8), decoded->mid(10, 32)};
}
}  // namespace

void verifySignature(const QString &publicKey, const QByteArray &data, const QString &signature) {
  const PublicKey key = parsePublicKey(publicKey);
  if (signature.isEmpty() && data.isEmpty()) return;
  const QStringList lines = signature.split(QLatin1Char('\n'));
  if (lines.size() < 4 || !lines[0].startsWith(QLatin1String("untrusted comment: ")) || !lines[2].startsWith(QLatin1String("trusted comment: "))) fail(QStringLiteral("update signature: Invalid encoding in minisign data"));
  auto line = [](QString text) {
    if (text.endsWith(QLatin1Char('\r'))) text.chop(1);
    return text;
  };
  const auto sigBytes = fromBase64(line(lines[1]).trimmed().toLatin1());
  const auto globalBytes = fromBase64(line(lines[3]).trimmed().toLatin1());
  if (!sigBytes || sigBytes->size() != 74 || !globalBytes || globalBytes->size() != 64) fail(QStringLiteral("update signature: Invalid encoding in minisign data"));
  const QByteArray algorithm = sigBytes->left(2), keyId = sigBytes->mid(2, 8), sig = sigBytes->mid(10, 64);
  if (algorithm != "ED") fail(QStringLiteral("update signature rejected: Legacy signatures are not accepted"));
  if (keyId != key.keyId) fail(QStringLiteral("update signature rejected: Incompatible key identifiers"));
  uint8_t hash[64];
  crypto_blake2b(hash, 64, reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size()));
  if (crypto_ed25519_check(reinterpret_cast<const uint8_t *>(sig.constData()), reinterpret_cast<const uint8_t *>(key.key.constData()), hash, 64) != 0) fail(QStringLiteral("update signature rejected: Signature verification failed"));
  const QByteArray trusted = line(lines[2]).mid(17).toUtf8();
  const QByteArray global = sig + trusted;
  if (crypto_ed25519_check(reinterpret_cast<const uint8_t *>(globalBytes->constData()), reinterpret_cast<const uint8_t *>(key.key.constData()), reinterpret_cast<const uint8_t *>(global.constData()), size_t(global.size())) != 0)
    fail(QStringLiteral("update signature rejected: Signature verification failed"));
}

namespace {
std::optional<std::vector<quint64>> versionParts(const QString &version) {
  const QString core = version.section(QRegularExpression(QStringLiteral("[-+]")), 0, 0);
  std::vector<quint64> parts;
  for (const QString &part : core.split(QLatin1Char('.'))) {
    bool ok = false;
    const quint64 value = part.toULongLong(&ok);
    if (!ok || part.startsWith(QLatin1Char('+')) || part.startsWith(QLatin1Char('-'))) return std::nullopt;
    parts.push_back(value);
  }
  if (parts.empty() || parts.size() > 4) return std::nullopt;
  return parts;
}

void allowedUrl(const QString &raw) {
  if (const auto f = qaFixture()) return qaUrl(raw, *f);
  const QUrl url(raw, QUrl::StrictMode);
  if (!url.isValid()) fail(url.errorString());
  const QString host = url.host();
  if (url.scheme() == QLatin1String("https"))
    for (const char *allowed : kAllowedHosts)
      if (host == QLatin1String(allowed)) return;
  // Isolated update tests serve the signed feed from loopback; the signature check is the same.
  if (qEnvironmentVariableIsSet("ROOM237_UPDATE_LOOPBACK") && url.scheme() == QLatin1String("http") && host == QLatin1String("127.0.0.1")) return;
  fail(QStringLiteral("Update URL is not allowed: %1").arg(raw));
}
}  // namespace

bool isNewer(const QString &candidate, const QString &current) {
  auto a = versionParts(candidate);
  if (!a) fail(QStringLiteral("Invalid update version"));
  auto b = versionParts(current);
  if (!b) fail(QStringLiteral("Invalid current version"));
  a->resize(4, 0);
  b->resize(4, 0);
  return *a > *b;
}

QString currentVersion() { return QStringLiteral(ROOM237_VERSION); }

QJsonObject check(const QString &manifest, const QString &signature, const QString &current, const QString &platform) {
  const auto fixture = qaFixture();
  verifySignature(fixture ? fixture->publicKey : QString::fromLatin1(kPublicKey), manifest.toUtf8(), signature);
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(manifest.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) fail(error.errorString());
  const QJsonObject parsed = document.object();
  const QString version = json::stringField(parsed.value(QLatin1String("version")), "version");
  const QString notes = parsed.value(QLatin1String("notes")).isString() ? parsed.value(QLatin1String("notes")).toString() : QString();
  const QJsonValue platforms = parsed.value(QLatin1String("platforms"));
  if (!platforms.isObject()) fail(QStringLiteral("missing field `platforms`"));
  QJsonObject unavailable{{QStringLiteral("available"), false}, {QStringLiteral("version"), version}, {QStringLiteral("notes"), notes}};
  if (!isNewer(version, current)) return unavailable;
  const QJsonValue entryValue = platforms.toObject().value(platform);
  if (!entryValue.isObject()) return unavailable;
  const QJsonObject entry = entryValue.toObject();
  const QString url = json::stringField(entry.value(QLatin1String("url")), "url");
  const QString sha256 = json::stringField(entry.value(QLatin1String("sha256")), "sha256");
  const quint64 size = json::unsignedField(entry.value(QLatin1String("size")), "size", ~0ull);
  const QString kind = json::stringField(entry.value(QLatin1String("kind")), "kind");
  allowedUrl(url);
  static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{64}$"));
  if (!hex.match(sha256).hasMatch()) fail(QStringLiteral("Invalid payload hash in update manifest"));
  if (size == 0 || size > kMaxPayload) fail(QStringLiteral("Invalid payload size in update manifest"));
  if (kind != QLatin1String("tar.gz") && kind != QLatin1String("appimage")) fail(QStringLiteral("Unknown payload kind: %1").arg(kind));
  return {{QStringLiteral("available"), true}, {QStringLiteral("version"), version}, {QStringLiteral("notes"), notes}, {QStringLiteral("url"), url}, {QStringLiteral("sha256"), sha256}, {QStringLiteral("size"), qint64(size)}, {QStringLiteral("kind"), kind}};
}

// --- tar.gz ------------------------------------------------------------------------------------------------------

namespace {
class Gunzip {
public:
  explicit Gunzip(const QString &p) : file_(p) {
    if (!file_.open(QIODevice::ReadOnly)) fail(file_.errorString());
    stream_ = {};
    if (inflateInit2(&stream_, 15 + 32) != Z_OK) fail(QStringLiteral("Cannot initialise gzip decoder"));
  }
  ~Gunzip() { inflateEnd(&stream_); }
  // Reads exactly `size` bytes or fails; returns false at a clean end of stream.
  bool read(char *out, size_t size) {
    size_t done = 0;
    while (done < size) {
      if (buffered_.size() > position_) {
        const size_t n = std::min(size - done, size_t(buffered_.size()) - position_);
        memcpy(out + done, buffered_.constData() + position_, n);
        position_ += n;
        done += n;
        continue;
      }
      if (finished_) {
        if (done == 0) return false;
        fail(QStringLiteral("Unexpected end of update archive"));
      }
      fill();
    }
    return true;
  }
  void skip(quint64 size) {
    char sink[65536];
    while (size > 0) {
      const size_t n = size_t(std::min<quint64>(size, sizeof sink));
      if (!read(sink, n)) fail(QStringLiteral("Unexpected end of update archive"));
      size -= n;
    }
  }
private:
  void fill() {
    buffered_.resize(1 << 20);
    position_ = 0;
    stream_.next_out = reinterpret_cast<Bytef *>(buffered_.data());
    stream_.avail_out = uInt(buffered_.size());
    while (stream_.avail_out == uInt(buffered_.size()) && !finished_) {
      if (stream_.avail_in == 0) {
        input_ = file_.read(1 << 20);
        if (input_.isEmpty()) {
          if (file_.error() != QFileDevice::NoError) fail(file_.errorString());
          fail(QStringLiteral("Unexpected end of update archive"));
        }
        stream_.next_in = reinterpret_cast<Bytef *>(input_.data());
        stream_.avail_in = uInt(input_.size());
      }
      const int rc = inflate(&stream_, Z_NO_FLUSH);
      if (rc == Z_STREAM_END) {
        // Concatenated gzip members continue the same archive.
        if (stream_.avail_in == 0 && file_.atEnd()) finished_ = true;
        else inflateReset(&stream_);
      } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
        fail(QStringLiteral("Corrupt update archive: %1").arg(QString::fromLatin1(stream_.msg ? stream_.msg : "inflate error")));
      }
    }
    buffered_.resize(qsizetype(buffered_.size() - qsizetype(stream_.avail_out)));
  }
  QFile file_;
  z_stream stream_;
  QByteArray input_, buffered_;
  size_t position_ = 0;
  bool finished_ = false;
};

quint64 octal(const char *field, size_t size) {
  // GNU base-256 for large sizes.
  if (uchar(field[0]) & 0x80) {
    quint64 value = uchar(field[0]) & 0x7f;
    for (size_t i = 1; i < size; ++i) value = (value << 8) | uchar(field[i]);
    return value;
  }
  quint64 value = 0;
  for (size_t i = 0; i < size && field[i]; ++i) {
    if (field[i] == ' ') continue;
    if (field[i] < '0' || field[i] > '7') break;
    value = value * 8 + quint64(field[i] - '0');
  }
  return value;
}
QString cstring(const char *field, size_t size) { return QString::fromUtf8(field, qsizetype(strnlen(field, size))); }

// Normal and "." components only, as the earlier unpacker accepted.
QStringList safeComponents(const QString &p) {
  if (p.startsWith(QLatin1Char('/')) || p.startsWith(QLatin1Char('\\')) || (p.size() > 1 && p[1] == QLatin1Char(':'))) return {};
  QStringList parts;
  for (const QString &part : p.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
    if (part == QLatin1String(".")) continue;
    if (part == QLatin1String("..") || part.contains(QLatin1Char('\\'))) return {};
    parts << part;
  }
  return parts;
}
}  // namespace

// Rejects anything that could leave the staging directory or create a special file.
void unpack(const QString &archive, const QString &destination) {
  Gunzip in(archive);
  quint64 written = 0;
  QString longName, longLink;
  QHash<QString, QString> pax;
  // Directory modes apply after every entry is written, so a read-only directory can still receive its files.
  std::vector<std::pair<QString, quint32>> directoryModes;
  for (;;) {
    char header[512];
    if (!in.read(header, 512)) break;
    if (std::all_of(header, header + 512, [](char c) { return c == 0; })) break;
    const char type = header[156];
    const quint64 size = octal(header + 124, 12);
    const quint64 padded = (size + 511) / 512 * 512;
    if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
      if (size > (1 << 20)) fail(QStringLiteral("Update archive header too large"));
      QByteArray data(qsizetype(padded), Qt::Uninitialized);
      if (!in.read(data.data(), size_t(padded))) fail(QStringLiteral("Unexpected end of update archive"));
      data.truncate(qsizetype(size));
      if (type == 'L') longName = QString::fromUtf8(data.left(data.indexOf('\0') < 0 ? data.size() : data.indexOf('\0')));
      else if (type == 'K') longLink = QString::fromUtf8(data.left(data.indexOf('\0') < 0 ? data.size() : data.indexOf('\0')));
      else if (type == 'x') {
        qsizetype at = 0;
        while (at < data.size()) {
          const qsizetype space = data.indexOf(' ', at);
          if (space < 0) break;
          const qsizetype length = data.mid(at, space - at).toLongLong();
          if (length <= 0 || at + length > data.size()) break;
          const QByteArray record = data.mid(space + 1, length - (space - at) - 2);
          const qsizetype equals = record.indexOf('=');
          if (equals > 0) pax.insert(QString::fromUtf8(record.left(equals)), QString::fromUtf8(record.mid(equals + 1)));
          at += length;
        }
      }
      continue;
    }
    QString name = cstring(header, 100);
    if (memcmp(header + 257, "ustar", 5) == 0) {
      const QString prefix = cstring(header + 345, 155);
      if (!prefix.isEmpty()) name = prefix + QLatin1Char('/') + name;
    }
    if (!longName.isEmpty()) name = longName;
    if (pax.contains(QStringLiteral("path"))) name = pax.value(QStringLiteral("path"));
    QString link = cstring(header + 157, 100);
    if (!longLink.isEmpty()) link = longLink;
    if (pax.contains(QStringLiteral("linkpath"))) link = pax.value(QStringLiteral("linkpath"));
    quint64 entrySize = size;
    if (pax.contains(QStringLiteral("size"))) entrySize = pax.value(QStringLiteral("size")).toULongLong();
    const quint64 entryPadded = (entrySize + 511) / 512 * 512;
    longName.clear();
    longLink.clear();
    pax.clear();
    const QStringList parts = safeComponents(name);
    if (parts.isEmpty() && name != QLatin1String(".") && name != QLatin1String("./")) fail(QStringLiteral("Unsafe path in update: %1").arg(name));
    const quint32 mode = quint32(octal(header + 100, 8));
    const QString target = parts.isEmpty() ? destination : path::join(destination, parts.join(kSeparator));
    if (type == '2') {
      qint64 depth = qint64(parts.size()) - 1;
      for (const QString &part : link.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String(".")) continue;
        if (part == QLatin1String("..")) --depth;
        else if (link.startsWith(QLatin1Char('/'))) fail(QStringLiteral("Unsafe link in update: %1").arg(name));
        else ++depth;
        if (depth < 0) fail(QStringLiteral("Unsafe link in update: %1").arg(name));
      }
      if (link.startsWith(QLatin1Char('/'))) fail(QStringLiteral("Unsafe link in update: %1").arg(name));
      in.skip(entryPadded);
      if (!parts.isEmpty()) {
        createDirs(path::parent(target));
#ifndef Q_OS_WIN
        if (::symlink(link.toUtf8().constData(), QFile::encodeName(target).constData()) != 0) fail(QStringLiteral("Cannot create link %1").arg(name));
#else
        fail(QStringLiteral("Unsupported entry in update: %1").arg(name));
#endif
      }
      continue;
    }
    if (type != '0' && type != '\0' && type != '5' && type != '7') fail(QStringLiteral("Unsupported entry in update: %1").arg(name));
    written += entrySize;
    if (written > kMaxUnpacked) fail(QStringLiteral("Update unpacks past the size limit"));
    if (type == '5') {
      createDirs(target);
      in.skip(entryPadded);
      directoryModes.emplace_back(target, mode);
      continue;
    }
    createDirs(path::parent(target));
    if (lstat(target).isSymlink) fail(QStringLiteral("Unsafe path in update: %1").arg(name));
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) fail(QStringLiteral("%1: %2").arg(target, out.errorString()));
    quint64 remaining = entrySize;
    char chunk[65536];
    while (remaining > 0) {
      const size_t n = size_t(std::min<quint64>(remaining, sizeof chunk));
      if (!in.read(chunk, n)) fail(QStringLiteral("Unexpected end of update archive"));
      if (out.write(chunk, qint64(n)) != qint64(n)) fail(out.errorString());
      remaining -= n;
    }
    out.close();
    in.skip(entryPadded - entrySize);
#ifndef Q_OS_WIN
    ::chmod(QFile::encodeName(target).constData(), mode & 07777);
#endif
  }
#ifndef Q_OS_WIN
  for (auto it = directoryModes.rbegin(); it != directoryModes.rend(); ++it) ::chmod(QFile::encodeName(it->first).constData(), it->second & 07777);
#endif
}

QJsonObject stage(const QJsonObject &plan) {
  const QString payload = json::stringField(plan.value(QLatin1String("payload")), "payload");
  const QString sha256 = json::stringField(plan.value(QLatin1String("sha256")), "sha256");
  const quint64 size = json::unsignedField(plan.value(QLatin1String("size")), "size", ~0ull);
  const QString kind = json::stringField(plan.value(QLatin1String("kind")), "kind");
  const QString install = json::stringField(plan.value(QLatin1String("install")), "install");
  const QString probe = json::stringField(plan.value(QLatin1String("probe")), "probe");
  const QString version = json::stringField(plan.value(QLatin1String("version")), "version");
  if (const auto f = qaFixture()) {
    const QString canonicalPayload = canonicalTaskPath(payload, qEnvironmentVariable("ROOM237_QA_FENCE"));
    if (install != f->install || !path::startsWith(canonicalPayload, path::join(f->profile, QStringLiteral("updates")))) fail(QStringLiteral("QA stage outside fixture"));
    if (path::isAbsolute(probe) || probe.split(QRegularExpression(QStringLiteral("[/\\\\]"))).contains(QStringLiteral(".."))) fail(QStringLiteral("QA probe must be relative inside staging"));
  }
  QFile file(payload);
  if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
  QCryptographicHash hasher(QCryptographicHash::Sha256);
  if (!hasher.addData(&file)) fail(file.errorString());
  const quint64 actual = quint64(file.size());
  file.close();
  if (actual != size) fail(QStringLiteral("Update size mismatch: %1 != %2").arg(actual).arg(size));
  if (toHex(hasher.result()).compare(sha256, Qt::CaseInsensitive) != 0) fail(QStringLiteral("Update hash mismatch"));
  const QString staged = sibling(install, QStringLiteral("update-new")), backup = sibling(install, QStringLiteral("update-old"));
  removePath(staged);
  if (kind == QLatin1String("tar.gz")) {
    createDirs(staged);
    try {
      unpack(payload, staged);
    } catch (const Error &) {
      try {
        removePath(staged);
      } catch (...) {
      }
      throw;
    }
  } else if (kind == QLatin1String("appimage")) {
    copyFile(payload, staged);
#ifndef Q_OS_WIN
    ::chmod(QFile::encodeName(staged).constData(), 0755);
#endif
  } else {
    fail(QStringLiteral("Unknown payload kind: %1").arg(kind));
  }
  const QString probePath = probe.isEmpty() ? staged : path::join(staged, probe);
  if (qaRun() && kind == QLatin1String("tar.gz") && !path::startsWith(canonical(probePath), canonical(staged))) fail(QStringLiteral("QA probe escaped staging"));
  if (!isFile(probePath)) {
    try {
      removePath(staged);
    } catch (...) {
    }
    fail(QStringLiteral("Update does not contain %1").arg(probe));
  }
  if (kind == QLatin1String("tar.gz")) {
    QString reported;
    try {
      reported = QString::fromUtf8(runChecked(probePath, {QStringLiteral("--version")}, 20000).stdoutBytes).trimmed();
    } catch (const Error &) {
    }
    if (reported != version) {
      try {
        removePath(staged);
      } catch (...) {
      }
      fail(QStringLiteral("Staged update reports version '%1', expected '%2'").arg(reported, version));
    }
  }
  return {{QStringLiteral("staged"), staged}, {QStringLiteral("backup"), backup}};
}

void launchHelper(const QJsonObject &plan) {
  const QString install = json::stringField(plan.value(QLatin1String("install")), "install");
  const QString staged = json::stringField(plan.value(QLatin1String("staged")), "staged");
  const QString backup = json::stringField(plan.value(QLatin1String("backup")), "backup");
  const QString healthFile = json::stringField(plan.value(QLatin1String("healthFile")), "healthFile");
  const QString resultFile = json::stringField(plan.value(QLatin1String("resultFile")), "resultFile");
  const quint64 waitPid = json::unsignedField(plan.value(QLatin1String("waitPid")), "waitPid", 0xffffffffu);
  const quint64 healthTimeout = json::unsignedField(plan.value(QLatin1String("healthTimeoutSecs")), "healthTimeoutSecs", ~0ull);
  const QJsonValue launchValue = plan.value(QLatin1String("launch"));
  if (!launchValue.isArray()) fail(QStringLiteral("missing field `launch`"));
  QStringList launch;
  for (const QJsonValue &part : launchValue.toArray()) launch << json::stringField(part, "launch");
  if (const auto f = qaFixture()) {
    const QStringList expected{f->executable, QStringLiteral("--root"), f->root, QStringLiteral("--profile"), f->profile, QStringLiteral("--legacy-profile"), f->legacy};
    if (install != f->install || staged != f->staging || backup != sibling(f->install, QStringLiteral("update-old")) || launch != expected || healthFile != path::join(f->profile, QStringLiteral("update-health")) ||
        resultFile != path::join(f->profile, QStringLiteral("update-result.json")) || waitPid == 0 || healthTimeout < 1 || healthTimeout > 300)
      fail(QStringLiteral("QA helper plan differs from fixture"));
  }
  const QString directory = path::parent(resultFile);
  if (directory.isEmpty()) fail(QStringLiteral("Result file has no directory"));
  createDirs(directory);
  const QString planPath = path::join(directory, QStringLiteral("update-plan.json"));
  QStringList launchJson;
  for (const QString &part : launch) launchJson << json::string(part);
  const QString text = json::Object()
                           .unum("waitPid", waitPid)
                           .str("install", install)
                           .str("staged", staged)
                           .str("backup", backup)
                           .raw("launch", QLatin1Char('[') + launchJson.join(QLatin1Char(',')) + QLatin1Char(']'))
                           .str("healthFile", healthFile)
                           .str("resultFile", resultFile)
                           .unum("healthTimeoutSecs", healthTimeout)
                           .compact();
  atomicWrite(planPath, text.toUtf8());
  // The helper is a separate executable without Qt, so its copy runs outside the install it replaces.
#ifdef Q_OS_WIN
  const QString source = path::join(QCoreApplication::applicationDirPath(), QStringLiteral("room237-core.exe"));
  const QString helper = path::join(directory, QStringLiteral("room237-update-helper.exe"));
#else
  const QString source = path::join(QCoreApplication::applicationDirPath(), QStringLiteral("room237-core"));
  const QString helper = path::join(directory, QStringLiteral("room237-update-helper"));
#endif
  removeFileIfExists(helper);
  copyFile(source, helper);
  QProcess process;
  process.setProgram(helper);
  process.setArguments({QStringLiteral("--apply-update"), planPath});
  process.setStandardInputFile(QProcess::nullDevice());
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000 | 0x00000008; });
#else
  process.setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
  qint64 pid = 0;
  if (!process.startDetached(&pid)) fail(process.errorString());
}

void cleanup(const QString &install) {
  if (const auto f = qaFixture(); f && install != f->install) fail(QStringLiteral("QA cleanup outside fixture"));
  const QString backup = sibling(install, QStringLiteral("update-old"));
  if (!exists(install) && exists(backup)) {
    // The swap stopped between its two renames.
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
      try {
        renameReplace(backup, install);
        break;
      } catch (const Error &error) {
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(15)) fail(QStringLiteral("%1 -> %2: %3").arg(backup, install, error.message()));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }
  }
  for (const char *suffix : {"update-new", "update-old", "update-failed"}) removePath(sibling(install, QString::fromLatin1(suffix)));
}

}  // namespace core::update
