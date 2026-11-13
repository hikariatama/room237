#pragma once
// Security hardening: encrypted roots. A root with ".room237-vault" keeps every original and preview encrypted on
// disk under random names, and its album databases hold encrypted rows. The master key exists only in memory while the
// root is unlocked; it is unwrapped from a key slot with the password (Argon2id) and a second factor.
#include "fs.h"

#include <QFile>
#include <QIODevice>
#include <QJsonObject>
#include <functional>
#include <memory>

namespace core::vault {

inline const QString kVaultFile = QStringLiteral(".room237-vault");
inline const QString kExtension = QStringLiteral("r237");
inline constexpr char kMagic[8] = {'R', '2', '3', '7', 'E', 'N', 'C', '1'};

// Bytes that are wiped when released.
class Secret {
public:
  Secret() = default;
  explicit Secret(QByteArray bytes) : bytes_(std::move(bytes)) {}
  Secret(const Secret &) = default;
  Secret &operator=(const Secret &) = default;
  ~Secret();
  void wipe();
  const QByteArray &bytes() const { return bytes_; }
  bool empty() const { return bytes_.isEmpty(); }
private:
  QByteArray bytes_;
};

QByteArray randomBytes(int size);
Secret argon2id(const QByteArray &password, const QByteArray &salt, quint32 memoryKiB, quint32 passes);
Secret derive(const Secret &key, const QByteArray &context, int size = 32);  // Keyed BLAKE2b.
QByteArray seal(const Secret &key, const QByteArray &plain, const QByteArray &ad = {});  // nonce | mac | cipher
std::optional<QByteArray> open(const Secret &key, const QByteArray &sealed, const QByteArray &ad = {});

// --- Key slots -----------------------------------------------------------------------------------------------------

// A second factor contributes S, the secret mixed with the password-derived key P.
struct Slot {
  QString kind;        // "macos-se", "windows-hello", "tpm2", "fido2", "recovery"
  QJsonObject params;  // Public data the factor needs to reproduce S (an SE key blob, a FIDO credential id...).
  QByteArray wrap;     // seal(KEK, MK)
};
struct VaultFile {
  int version = 1;
  QString id;
  QByteArray salt;
  quint32 memoryKiB = 65536, passes = 3;
  std::vector<Slot> keySlots;
  QByteArray decoyTag;  // Fixed size; random bytes when no decoy password is set.
  QString state;        // "encrypting", "encrypted", "decrypting"
  QJsonObject toJson() const;
  static VaultFile fromJson(const QJsonObject &object);
  static std::optional<VaultFile> load(const QString &root);
  void save(const QString &root) const;
};
Secret slotKey(const Secret &p, const QString &kind, const Secret &s);
QByteArray decoyTag(const Secret &p);

// Recovery key: 160 random bits shown as eight groups of five base32 characters plus a checksum group.
QString newRecoveryKey();
std::optional<Secret> parseRecoveryKey(const QString &text);

// --- Unlocked roots --------------------------------------------------------------------------------------------------

struct Keys {
  Secret master, rows, names;
  QString root, id;
};
void unlock(const QString &root, Keys keys);
void lock(const QString &root);
void lockAll();
// The keys of the vault that contains `path`, if it is unlocked. `encrypted` tells whether the path is inside a
// vault root at all (locked or not).
std::shared_ptr<const Keys> keysFor(const QString &path, bool *inVault = nullptr);
std::optional<QString> vaultRoot(const QString &path);  // Nearest ancestor with a vault file.
void forgetRoots();  // After a vault file was created or removed.
bool isVaultPath(const QString &path);
void requireUnlocked(const QString &path);

// --- Encrypted files ---------------------------------------------------------------------------------------------

struct FileMeta {
  QString name;  // The original file name.
  qint64 mtimeNs = 0;
  std::optional<qint64> birthSec;
  quint64 size = 0;
  QByteArray sha256;
  QJsonObject xattrs;  // name -> base64 value
};
bool isEncryptedFile(const QString &path);
QString newPhysicalName();  // "<26 base32>.r237"
// Encrypts `plain` into a new file `target` (which must not exist). Returns the stored metadata.
FileMeta encryptFile(const Keys &keys, const QString &plain, const QString &target, const FileMeta &meta);
void encryptBytes(const Keys &keys, const QByteArray &plain, const QString &target, const FileMeta &meta, bool replace = false);
FileMeta readMeta(const Keys &keys, const QString &path);
void writeMeta(const Keys &keys, const QString &path, const FileMeta &meta);  // Keeps the file's mtime.
QByteArray decryptAll(const Keys &keys, const QString &path, qint64 limit = -1);
void decryptToFile(const Keys &keys, const QString &path, const QString &target);  // target must not exist

// Random-access plaintext of an encrypted file; for decoders and the video player.
class DecryptingDevice : public QIODevice {
public:
  DecryptingDevice(std::shared_ptr<const Keys> keys, const QString &path);
  ~DecryptingDevice() override;
  bool open(OpenMode mode) override;
  bool isSequential() const override { return false; }
  qint64 size() const override;
  bool seek(qint64 pos) override;
  const FileMeta &meta() const { return meta_; }
protected:
  qint64 readData(char *data, qint64 maxSize) override;
  qint64 writeData(const char *, qint64) override { return -1; }
private:
  bool loadChunk(quint64 index);
  std::shared_ptr<const Keys> keys_;
  QString path_;
  std::unique_ptr<QFile> file_;
  Secret fileKey_;
  QByteArray salt_;
  FileMeta meta_;
  quint64 chunkSize_ = 0, dataOffset_ = 0;
  qint64 cachedChunk_ = -1;
  QByteArray chunk_;
};
// Plaintext of `path` when it is an encrypted file of an unlocked vault; nullopt for an ordinary file.
std::unique_ptr<QIODevice> openPlain(const QString &path);
QByteArray readPlain(const QString &path, qint64 limit = -1);

// Values of extended attributes on encrypted files are sealed with the file's own key.
QByteArray sealAttribute(const Keys &keys, const QString &path, const QByteArray &value);
std::optional<QByteArray> openAttribute(const Keys &keys, const QString &path, const QByteArray &sealed);

}  // namespace core::vault
