#include "vault.h"

#include "json.h"
#include "util.h"
#include "xattr.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <monocypher.h>
#include <mutex>
#include <shared_mutex>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace core::vault {

namespace {
constexpr quint32 kChunkSize = 64 * 1024;
constexpr qint64 kSaltOffset = 16, kSaltSize = 32;
constexpr qint64 kMetaOffset = 48, kMetaSlot = 4096, kMetaCipher = kMetaSlot - 4 - 24 - 16;
constexpr qint64 kDataOffset = kMetaOffset + 2 * kMetaSlot;
const char kBase32[] = "abcdefghijklmnopqrstuvwxyz234567";

const uint8_t *u8(const QByteArray &bytes) { return reinterpret_cast<const uint8_t *>(bytes.constData()); }
uint8_t *u8(QByteArray &bytes) { return reinterpret_cast<uint8_t *>(bytes.data()); }

QString base32(const QByteArray &bytes, bool upper) {
  QString out;
  quint32 buffer = 0;
  int bits = 0;
  for (char c : bytes) {
    buffer = (buffer << 8) | uchar(c);
    bits += 8;
    while (bits >= 5) {
      out += QLatin1Char(kBase32[(buffer >> (bits - 5)) & 31]);
      bits -= 5;
    }
  }
  if (bits > 0) out += QLatin1Char(kBase32[(buffer << (5 - bits)) & 31]);
  return upper ? out.toUpper() : out;
}
std::optional<QByteArray> unbase32(const QString &text) {
  QByteArray out;
  quint32 buffer = 0;
  int bits = 0;
  for (QChar c : text.toLower()) {
    const char *at = strchr(kBase32, c.toLatin1());
    if (!c.toLatin1() || !at) return std::nullopt;
    buffer = (buffer << 5) | quint32(at - kBase32);
    bits += 5;
    if (bits >= 8) {
      out += char((buffer >> (bits - 8)) & 0xff);
      bits -= 8;
    }
  }
  return out;
}

void le32(QByteArray &out, qint64 at, quint32 value) {
  for (int i = 0; i < 4; ++i) out[at + i] = char((value >> (8 * i)) & 0xff);
}
quint32 readLe32(const QByteArray &in, qint64 at) {
  quint32 value = 0;
  for (int i = 0; i < 4; ++i) value |= quint32(uchar(in[at + i])) << (8 * i);
  return value;
}

QByteArray chunkNonce(quint64 index, bool last) {
  QByteArray nonce(24, '\0');
  for (int i = 0; i < 8; ++i) nonce[i] = char((index >> (8 * i)) & 0xff);
  nonce[8] = last ? 1 : 0;
  return nonce;
}

Secret fileKey(const Keys &keys, const QByteArray &salt) { return derive(keys.master, QByteArrayLiteral("room237 file v1") + salt); }

QByteArray metaPlain(const FileMeta &meta) {
  QJsonObject o{{QStringLiteral("name"), meta.name}, {QStringLiteral("mtimeNs"), QString::number(meta.mtimeNs)}, {QStringLiteral("size"), QString::number(meta.size)}, {QStringLiteral("sha256"), QString::fromLatin1(meta.sha256.toHex())}};
  if (meta.birthSec) o.insert(QStringLiteral("birth"), QString::number(*meta.birthSec));
  if (!meta.xattrs.isEmpty()) o.insert(QStringLiteral("xattrs"), meta.xattrs);
  QByteArray text = QJsonDocument(o).toJson(QJsonDocument::Compact);
  if (text.size() > kMetaCipher) {
    // Attributes are kept only while they fit; the name, times and digest always do.
    o.remove(QStringLiteral("xattrs"));
    text = QJsonDocument(o).toJson(QJsonDocument::Compact);
  }
  if (text.size() > kMetaCipher) fail(QStringLiteral("File name is too long to encrypt"));
  return text + QByteArray(kMetaCipher - text.size(), ' ');
}
FileMeta parseMeta(const QByteArray &plain) {
  const QJsonObject o = QJsonDocument::fromJson(plain.trimmed()).object();
  FileMeta meta;
  meta.name = o.value(QLatin1String("name")).toString();
  meta.mtimeNs = o.value(QLatin1String("mtimeNs")).toString().toLongLong();
  meta.size = o.value(QLatin1String("size")).toString().toULongLong();
  meta.sha256 = QByteArray::fromHex(o.value(QLatin1String("sha256")).toString().toLatin1());
  if (o.contains(QLatin1String("birth"))) meta.birthSec = o.value(QLatin1String("birth")).toString().toLongLong();
  meta.xattrs = o.value(QLatin1String("xattrs")).toObject();
  return meta;
}

QByteArray sealMetaSlot(const Secret &key, const QByteArray &salt, quint32 generation, const FileMeta &meta) {
  QByteArray slot(kMetaSlot, '\0');
  le32(slot, 0, generation);
  const QByteArray nonce = randomBytes(24);
  memcpy(slot.data() + 4, nonce.constData(), 24);
  QByteArray ad = salt;
  ad.append(slot.left(4));
  const QByteArray plain = metaPlain(meta);
  QByteArray cipher(plain.size(), '\0');
  uint8_t mac[16];
  crypto_aead_lock(u8(cipher), mac, u8(key.bytes()), u8(nonce), u8(ad), size_t(ad.size()), u8(plain), size_t(plain.size()));
  memcpy(slot.data() + 28, mac, 16);
  memcpy(slot.data() + 44, cipher.constData(), size_t(cipher.size()));
  return slot;
}
std::optional<std::pair<quint32, FileMeta>> openMetaSlot(const Secret &key, const QByteArray &salt, const QByteArray &slot) {
  if (slot.size() != kMetaSlot) return std::nullopt;
  const quint32 generation = readLe32(slot, 0);
  if (generation == 0) return std::nullopt;
  QByteArray ad = salt;
  ad.append(slot.left(4));
  QByteArray plain(kMetaCipher, '\0');
  if (crypto_aead_unlock(u8(plain), reinterpret_cast<const uint8_t *>(slot.constData() + 28), u8(key.bytes()), reinterpret_cast<const uint8_t *>(slot.constData() + 4), u8(ad), size_t(ad.size()),
                         reinterpret_cast<const uint8_t *>(slot.constData() + 44), size_t(kMetaCipher)) != 0)
    return std::nullopt;
  return std::make_pair(generation, parseMeta(plain));
}

struct Header {
  QByteArray salt;
  quint32 chunkSize = kChunkSize;
  quint32 generation = 0;
  FileMeta meta;
  int activeSlot = 0;
};
Header readHeader(const Keys &keys, QFile &file) {
  file.seek(0);
  const QByteArray head = file.read(kDataOffset);
  if (head.size() != kDataOffset || memcmp(head.constData(), kMagic, 8) != 0) fail(QStringLiteral("Not an encrypted Room237 file"));
  if (uchar(head[8]) != 1) fail(QStringLiteral("Unsupported encrypted file version"));
  Header header;
  header.chunkSize = readLe32(head, 12);
  if (header.chunkSize == 0 || header.chunkSize > (16u << 20)) fail(QStringLiteral("Invalid encrypted file"));
  header.salt = head.mid(kSaltOffset, kSaltSize);
  const Secret key = derive(fileKey(keys, header.salt), QByteArrayLiteral("meta"));
  const auto a = openMetaSlot(key, header.salt, head.mid(kMetaOffset, kMetaSlot));
  const auto b = openMetaSlot(key, header.salt, head.mid(kMetaOffset + kMetaSlot, kMetaSlot));
  if (!a && !b) fail(QStringLiteral("Encrypted file belongs to another key or is damaged"));
  const bool useB = b && (!a || b->first > a->first);
  header.generation = useB ? b->first : a->first;
  header.meta = useB ? b->second : a->second;
  header.activeSlot = useB ? 1 : 0;
  return header;
}

void setMtime(const QString &p, qint64 mtimeNs) {
#ifndef Q_OS_WIN
  struct timespec times[2];
  times[0].tv_sec = 0;
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = time_t(mtimeNs / 1000000000LL);
  times[1].tv_nsec = long(mtimeNs % 1000000000LL);
  utimensat(AT_FDCWD, QFile::encodeName(p).constData(), times, 0);
#else
  QFile file(p);
  if (file.open(QIODevice::ReadWrite)) file.setFileTime(QDateTime::fromMSecsSinceEpoch(mtimeNs / 1000000), QFileDevice::FileModificationTime);
#endif
}

void writeStream(const Keys &keys, QIODevice &input, quint64 size, const QString &target, FileMeta meta, bool hashInput, bool replace = false) {
  TempFile temporary(path::parent(target), QStringLiteral(".room237-vault-tmp-"));
  QFile out(temporary.path());
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) fail(out.errorString());
  const QByteArray salt = randomBytes(int(kSaltSize));
  const Secret key = fileKey(keys, salt);
  const Secret dataKey = derive(key, QByteArrayLiteral("data"));
  QByteArray head(kDataOffset, '\0');
  memcpy(head.data(), kMagic, 8);
  head[8] = 1;
  le32(head, 12, kChunkSize);
  memcpy(head.data() + kSaltOffset, salt.constData(), kSaltSize);
  if (out.write(head) != head.size()) fail(out.errorString());
  QCryptographicHash hasher(QCryptographicHash::Sha256);
  quint64 written = 0, index = 0;
  for (;;) {
    QByteArray plain = input.read(kChunkSize);
    if (plain.isEmpty()) break;
    if (hashInput) hasher.addData(plain);
    written += quint64(plain.size());
    const bool last = written >= size;
    QByteArray cipher(plain.size(), '\0');
    uint8_t mac[16];
    const QByteArray nonce = chunkNonce(index++, last);
    crypto_aead_lock(u8(cipher), mac, u8(dataKey.bytes()), u8(nonce), u8(salt), size_t(salt.size()), u8(plain), size_t(plain.size()));
    crypto_wipe(plain.data(), size_t(plain.size()));
    if (out.write(cipher) != cipher.size() || out.write(reinterpret_cast<const char *>(mac), 16) != 16) fail(out.errorString());
    if (last) break;
  }
  if (written != size) fail(QStringLiteral("File changed while it was encrypted"));
  meta.size = size;
  if (hashInput) meta.sha256 = hasher.result();
  out.seek(kMetaOffset);
  const Secret metaKey = derive(key, QByteArrayLiteral("meta"));
  if (out.write(sealMetaSlot(metaKey, salt, 1, meta)) != kMetaSlot) fail(out.errorString());
  out.close();
  temporary.sync();
  if (replace) temporary.persist(target);
  else temporary.persistNoClobber(target);
  if (meta.mtimeNs) setMtime(target, meta.mtimeNs);
}
}  // namespace

Secret::~Secret() { wipe(); }
void Secret::wipe() {
  if (!bytes_.isEmpty() && !bytes_.isDetached()) {
    bytes_ = QByteArray();
    return;
  }
  if (!bytes_.isEmpty()) crypto_wipe(bytes_.data(), size_t(bytes_.size()));
  bytes_.clear();
}

QByteArray randomBytes(int size) {
  QByteArray out(size, Qt::Uninitialized);
  QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(out.data()), reinterpret_cast<quint32 *>(out.data()) + size / 4);
  for (int i = size / 4 * 4; i < size; ++i) out[i] = char(QRandomGenerator::system()->generate() & 0xff);
  return out;
}

Secret argon2id(const QByteArray &password, const QByteArray &salt, quint32 memoryKiB, quint32 passes) {
  // Monocypher fills the work area itself; value-initializing it would add a pass over the whole area.
  const size_t workSize = size_t(memoryKiB) * 1024;
  std::unique_ptr<uint8_t[]> work(new uint8_t[workSize]);
  QByteArray hash(32, '\0');
  crypto_argon2_config config{CRYPTO_ARGON2_ID, memoryKiB, passes, 1};
  crypto_argon2_inputs inputs{u8(password), u8(salt), uint32_t(password.size()), uint32_t(salt.size())};
  crypto_argon2(u8(hash), 32, work.get(), config, inputs, crypto_argon2_no_extras);
  crypto_wipe(work.get(), workSize);
  return Secret(hash);
}

Secret derive(const Secret &key, const QByteArray &context, int size) {
  QByteArray out(size, '\0');
  crypto_blake2b_keyed(u8(out), size_t(size), u8(key.bytes()), size_t(key.bytes().size()), u8(context), size_t(context.size()));
  return Secret(out);
}

QByteArray seal(const Secret &key, const QByteArray &plain, const QByteArray &ad) {
  const QByteArray nonce = randomBytes(24);
  QByteArray out(24 + 16 + plain.size(), '\0');
  memcpy(out.data(), nonce.constData(), 24);
  crypto_aead_lock(u8(out) + 40, u8(out) + 24, u8(key.bytes()), u8(nonce), u8(ad), size_t(ad.size()), u8(plain), size_t(plain.size()));
  return out;
}

std::optional<QByteArray> open(const Secret &key, const QByteArray &sealed, const QByteArray &ad) {
  if (sealed.size() < 40 || key.bytes().size() != 32) return std::nullopt;
  QByteArray plain(sealed.size() - 40, '\0');
  if (crypto_aead_unlock(u8(plain), u8(sealed) + 24, u8(key.bytes()), u8(sealed), u8(ad), size_t(ad.size()), u8(sealed) + 40, size_t(plain.size())) != 0) return std::nullopt;
  return plain;
}

// --- Vault file ----------------------------------------------------------------------------------------------------

QJsonObject VaultFile::toJson() const {
  QJsonArray slotArray;
  for (const Slot &slot : keySlots) slotArray.append(QJsonObject{{QStringLiteral("kind"), slot.kind}, {QStringLiteral("params"), slot.params}, {QStringLiteral("wrap"), QString::fromLatin1(slot.wrap.toBase64())}});
  return {{QStringLiteral("version"), version},
          {QStringLiteral("id"), id},
          {QStringLiteral("kdf"), QJsonObject{{QStringLiteral("alg"), QStringLiteral("argon2id")}, {QStringLiteral("salt"), QString::fromLatin1(salt.toBase64())}, {QStringLiteral("memoryKiB"), qint64(memoryKiB)}, {QStringLiteral("passes"), qint64(passes)}}},
          {QStringLiteral("slots"), slotArray},
          {QStringLiteral("decoy"), QString::fromLatin1(decoyTag.toBase64())},
          {QStringLiteral("state"), state}};
}

VaultFile VaultFile::fromJson(const QJsonObject &o) {
  VaultFile v;
  v.version = int(json::unsignedField(o.value(QLatin1String("version")), "version", 255));
  if (v.version != 1) fail(QStringLiteral("Unsupported vault version"));
  v.id = json::stringField(o.value(QLatin1String("id")), "id");
  const QJsonObject kdf = o.value(QLatin1String("kdf")).toObject();
  if (kdf.value(QLatin1String("alg")).toString() != QLatin1String("argon2id")) fail(QStringLiteral("Unsupported key derivation"));
  v.salt = QByteArray::fromBase64(kdf.value(QLatin1String("salt")).toString().toLatin1());
  v.memoryKiB = quint32(json::unsignedField(kdf.value(QLatin1String("memoryKiB")), "memoryKiB", 4u << 20));
  v.passes = quint32(json::unsignedField(kdf.value(QLatin1String("passes")), "passes", 64));
  if (v.salt.size() < 16 || v.memoryKiB < 8192 || v.passes < 1) fail(QStringLiteral("Invalid key derivation parameters"));
  for (const QJsonValue &value : o.value(QLatin1String("slots")).toArray()) {
    const QJsonObject s = value.toObject();
    v.keySlots.push_back({json::stringField(s.value(QLatin1String("kind")), "kind"), s.value(QLatin1String("params")).toObject(), QByteArray::fromBase64(s.value(QLatin1String("wrap")).toString().toLatin1())});
  }
  v.decoyTag = QByteArray::fromBase64(o.value(QLatin1String("decoy")).toString().toLatin1());
  v.state = o.value(QLatin1String("state")).toString();
  return v;
}

std::optional<VaultFile> VaultFile::load(const QString &root) {
  const QString file = path::join(root, kVaultFile);
  if (!lstat(file).isFile) return std::nullopt;
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(readAll(file, 1 << 20), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) fail(QStringLiteral("The vault file of this root is damaged"));
  return fromJson(document.object());
}

void VaultFile::save(const QString &root) const {
  atomicWriteExisting(path::join(root, kVaultFile), QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
  forgetRoots();
}

Secret slotKey(const Secret &p, const QString &kind, const Secret &s) { return derive(p, QByteArrayLiteral("room237 slot v1|") + kind.toUtf8() + '|' + s.bytes()); }
QByteArray decoyTag(const Secret &p) { return derive(p, QByteArrayLiteral("room237 decoy v1")).bytes(); }

QString newRecoveryKey() {
  const QByteArray key = randomBytes(20);
  QByteArray check(2, '\0');
  crypto_blake2b(u8(check), 2, u8(key), size_t(key.size()));
  const QString text = base32(key + check, true);
  QStringList groups;
  for (int i = 0; i < text.size(); i += 4) groups << text.mid(i, 4);
  return groups.join(QLatin1Char('-'));
}

std::optional<Secret> parseRecoveryKey(const QString &text) {
  QString compact;
  for (QChar c : text)
    if (c.isLetterOrNumber()) compact += c;
  const auto bytes = unbase32(compact);
  if (!bytes || bytes->size() < 22) return std::nullopt;
  const QByteArray key = bytes->left(20), check = bytes->mid(20, 2);
  QByteArray expected(2, '\0');
  crypto_blake2b(u8(expected), 2, u8(key), size_t(key.size()));
  if (expected != check) return std::nullopt;
  return Secret(key);
}

// --- Registry of unlocked roots ------------------------------------------------------------------------------------

namespace {
std::shared_mutex registryMutex;
std::vector<std::pair<QStringList, std::shared_ptr<Keys>>> unlocked;
std::mutex rootCacheMutex;
QHash<QString, std::optional<QString>> rootCache;
}  // namespace

void unlock(const QString &root, Keys keys) {
  QStringList spellings{root};
  if (const auto real = tryCanonical(root); real && *real != root) spellings << *real;
  keys.root = root;
  auto shared = std::make_shared<Keys>(std::move(keys));
  std::unique_lock lock(registryMutex);
  unlocked.erase(std::remove_if(unlocked.begin(), unlocked.end(), [&](const auto &entry) { return entry.second->root == root; }), unlocked.end());
  unlocked.emplace_back(spellings, shared);
  std::lock_guard cache(rootCacheMutex);
  rootCache.clear();
}

void lock(const QString &root) {
  std::vector<std::shared_ptr<Keys>> released;
  {
    std::unique_lock lock(registryMutex);
    for (auto it = unlocked.begin(); it != unlocked.end();) {
      if (it->second->root == root || it->first.contains(root)) {
        released.push_back(it->second);
        it = unlocked.erase(it);
      } else {
        ++it;
      }
    }
  }
  // Readers that still hold the keys (an open decrypting device) finish with their own reference; the last one to
  // release them wipes the bytes.
  released.clear();
}

void lockAll() {
  QStringList roots;
  {
    std::shared_lock lock(registryMutex);
    for (const auto &entry : unlocked) roots << entry.second->root;
  }
  for (const QString &root : roots) lock(root);
}

void forgetRoots() {
  std::lock_guard cache(rootCacheMutex);
  rootCache.clear();
}

std::optional<QString> vaultRoot(const QString &p) {
  const QString dir = isDir(p) ? p : path::parent(p);
  {
    std::lock_guard cache(rootCacheMutex);
    if (auto it = rootCache.constFind(dir); it != rootCache.cend()) return *it;
  }
  std::optional<QString> found;
  for (const QString &ancestor : path::ancestors(dir))
    if (lstat(path::join(ancestor, kVaultFile)).isFile) {
      found = ancestor;
      break;
    }
  std::lock_guard cache(rootCacheMutex);
  if (rootCache.size() > 4096) rootCache.clear();
  rootCache.insert(dir, found);
  return found;
}

bool isVaultPath(const QString &p) { return vaultRoot(p).has_value(); }

std::shared_ptr<const Keys> keysFor(const QString &p, bool *inVault) {
  {
    std::shared_lock lock(registryMutex);
    for (const auto &[spellings, keys] : unlocked)
      for (const QString &root : spellings)
        if (path::startsWith(p, root)) {
          if (inVault) *inVault = true;
          return keys;
        }
  }
  if (inVault) *inVault = isVaultPath(p);
  return nullptr;
}

void requireUnlocked(const QString &p) {
  bool inVault = false;
  if (!keysFor(p, &inVault) && inVault) fail(QStringLiteral("This library is locked"));
}

// --- Encrypted files ---------------------------------------------------------------------------------------------

bool isEncryptedFile(const QString &p) {
  QFile file(p);
  if (!file.open(QIODevice::ReadOnly)) return false;
  return file.read(8) == QByteArray(kMagic, 8);
}

QString newPhysicalName() { return base32(randomBytes(16), false) + QLatin1Char('.') + kExtension; }

FileMeta encryptFile(const Keys &keys, const QString &plain, const QString &target, const FileMeta &meta) {
  QFile input(plain);
  if (!input.open(QIODevice::ReadOnly)) fail(input.errorString());
  FileMeta stored = meta;
  writeStream(keys, input, quint64(input.size()), target, stored, true);
  return readMeta(keys, target);
}

void encryptBytes(const Keys &keys, const QByteArray &plain, const QString &target, const FileMeta &meta, bool replace) {
  QByteArray copy = plain;
  QBuffer buffer(&copy);
  buffer.open(QIODevice::ReadOnly);
  writeStream(keys, buffer, quint64(plain.size()), target, meta, true, replace);
}

FileMeta readMeta(const Keys &keys, const QString &p) {
  QFile file(p);
  if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
  return readHeader(keys, file).meta;
}

void writeMeta(const Keys &keys, const QString &p, const FileMeta &meta) {
  const FileInfo before = stat(p);
  QFile file(p);
  if (!file.open(QIODevice::ReadWrite)) fail(file.errorString());
  const Header header = readHeader(keys, file);
  const Secret key = derive(fileKey(keys, header.salt), QByteArrayLiteral("meta"));
  FileMeta next = meta;
  next.size = header.meta.size;
  next.sha256 = header.meta.sha256;
  // The inactive slot takes the new generation; a crash midway leaves the previous one readable.
  file.seek(kMetaOffset + (header.activeSlot == 0 ? kMetaSlot : 0));
  if (file.write(sealMetaSlot(key, header.salt, header.generation + 1, next)) != kMetaSlot) fail(file.errorString());
  file.flush();
  file.close();
  syncFile(p);
  setMtime(p, before.mtimeNs);
}

QByteArray decryptAll(const Keys &keys, const QString &p, qint64 limit) {
  DecryptingDevice device(std::make_shared<Keys>(keys), p);
  if (!device.open(QIODevice::ReadOnly)) fail(device.errorString());
  if (limit >= 0 && device.size() > limit) fail(QStringLiteral("Encrypted file is larger than expected"));
  QByteArray plain = device.readAll();
  if (plain.size() != device.size()) fail(device.errorString().isEmpty() ? QStringLiteral("Encrypted file is damaged") : device.errorString());
  return plain;
}

void decryptToFile(const Keys &keys, const QString &p, const QString &target) {
  DecryptingDevice device(std::make_shared<Keys>(keys), p);
  if (!device.open(QIODevice::ReadOnly)) fail(device.errorString());
  TempFile temporary(path::parent(target), QStringLiteral(".room237-vault-tmp-"));
  QFile out(temporary.path());
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) fail(out.errorString());
  QCryptographicHash hasher(QCryptographicHash::Sha256);
  for (;;) {
    QByteArray chunk = device.read(kChunkSize);
    if (chunk.isEmpty()) break;
    hasher.addData(chunk);
    if (out.write(chunk) != chunk.size()) fail(out.errorString());
    crypto_wipe(chunk.data(), size_t(chunk.size()));
  }
  if (device.pos() != device.size()) fail(QStringLiteral("Encrypted file is damaged: %1").arg(p));
  out.close();
  if (!device.meta().sha256.isEmpty() && hasher.result() != device.meta().sha256) fail(QStringLiteral("Decrypted content does not match its digest: %1").arg(p));
  temporary.sync();
  temporary.persistNoClobber(target);
  setMtime(target, device.meta().mtimeNs);
}

DecryptingDevice::DecryptingDevice(std::shared_ptr<const Keys> keys, const QString &p) : keys_(std::move(keys)), path_(p) {}
DecryptingDevice::~DecryptingDevice() {
  if (!chunk_.isEmpty()) crypto_wipe(chunk_.data(), size_t(chunk_.size()));
}
bool DecryptingDevice::open(OpenMode mode) {
  if (mode & WriteOnly) return false;
  file_ = std::make_unique<QFile>(path_);
  if (!file_->open(QIODevice::ReadOnly)) {
    setErrorString(file_->errorString());
    return false;
  }
  try {
    const Header header = readHeader(*keys_, *file_);
    salt_ = header.salt;
    meta_ = header.meta;
    chunkSize_ = header.chunkSize;
    dataOffset_ = quint64(kDataOffset);
    fileKey_ = derive(fileKey(*keys_, salt_), QByteArrayLiteral("data"));
  } catch (const Error &error) {
    setErrorString(error.message());
    return false;
  }
  return QIODevice::open(mode | Unbuffered);
}
qint64 DecryptingDevice::size() const { return qint64(meta_.size); }
bool DecryptingDevice::seek(qint64 pos) {
  if (pos < 0 || pos > size()) return false;
  return QIODevice::seek(pos);
}
bool DecryptingDevice::loadChunk(quint64 index) {
  if (cachedChunk_ == qint64(index)) return true;
  const quint64 chunks = meta_.size == 0 ? 0 : (meta_.size + chunkSize_ - 1) / chunkSize_;
  if (index >= chunks) return false;
  const quint64 plainSize = index + 1 == chunks ? meta_.size - index * chunkSize_ : chunkSize_;
  if (!file_->seek(qint64(dataOffset_ + index * (chunkSize_ + 16)))) return false;
  const QByteArray cipher = file_->read(qint64(plainSize + 16));
  if (cipher.size() != qint64(plainSize + 16)) {
    setErrorString(QStringLiteral("Encrypted file is truncated"));
    return false;
  }
  chunk_.resize(qsizetype(plainSize));
  const QByteArray nonce = chunkNonce(index, index + 1 == chunks);
  if (crypto_aead_unlock(u8(chunk_), u8(cipher) + plainSize, u8(fileKey_.bytes()), u8(nonce), u8(salt_), size_t(salt_.size()), u8(cipher), size_t(plainSize)) != 0) {
    setErrorString(QStringLiteral("Encrypted file is damaged"));
    cachedChunk_ = -1;
    return false;
  }
  cachedChunk_ = qint64(index);
  return true;
}
qint64 DecryptingDevice::readData(char *data, qint64 maxSize) {
  qint64 done = 0;
  while (done < maxSize) {
    const quint64 position = quint64(pos() + done);
    if (position >= meta_.size) break;
    const quint64 index = position / chunkSize_;
    if (!loadChunk(index)) return done > 0 ? done : -1;
    const quint64 offset = position % chunkSize_;
    const qint64 n = std::min<qint64>(maxSize - done, qint64(chunk_.size()) - qint64(offset));
    memcpy(data + done, chunk_.constData() + offset, size_t(n));
    done += n;
  }
  return done;
}

std::unique_ptr<QIODevice> openPlain(const QString &p) {
  if (!isEncryptedFile(p)) return nullptr;
  auto keys = keysFor(p);
  if (!keys) fail(QStringLiteral("This library is locked"));
  auto device = std::make_unique<DecryptingDevice>(keys, p);
  if (!device->open(QIODevice::ReadOnly)) fail(device->errorString());
  return device;
}

QByteArray readPlain(const QString &p, qint64 limit) {
  if (auto device = openPlain(p)) {
    const qint64 want = limit >= 0 ? std::min(limit, device->size()) : device->size();
    QByteArray plain = device->read(want);
    if (plain.size() != want) fail(device->errorString().isEmpty() ? QStringLiteral("Encrypted file is damaged") : device->errorString());
    return plain;
  }
  return readAll(p, limit);
}

QByteArray sealAttribute(const Keys &keys, const QString &p, const QByteArray &value) {
  return "r237:" + seal(derive(keys.names, QByteArrayLiteral("attribute|") + path::fileName(p).toUtf8()), value).toBase64();
}
std::optional<QByteArray> openAttribute(const Keys &keys, const QString &p, const QByteArray &sealed) {
  if (!sealed.startsWith("r237:")) return std::nullopt;
  return open(derive(keys.names, QByteArrayLiteral("attribute|") + path::fileName(p).toUtf8()), QByteArray::fromBase64(sealed.mid(5)));
}

}  // namespace core::vault
