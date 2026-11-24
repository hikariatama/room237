#pragma once
// Paths, file identity, leases and crash-safe writes. Paths are QStrings joined with the native separator, as the
// previous core produced them, so every path the interface receives keeps its spelling.
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QFile>
#include <functional>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace core {
class FileAccess;

struct Error : std::runtime_error {
  explicit Error(const QString &message) : std::runtime_error(message.toStdString()) {}
  QString message() const { return QString::fromStdString(what()); }
};
[[noreturn]] inline void fail(const QString &message) { throw Error(message); }

#ifdef Q_OS_WIN
inline constexpr QChar kSeparator = QLatin1Char('\\');
#else
inline constexpr QChar kSeparator = QLatin1Char('/');
#endif

namespace path {
bool isSeparator(QChar c);
QString join(const QString &base, const QString &name);
QString parent(const QString &path);  // Empty when there is none.
QString fileName(const QString &path);
QString extension(const QString &path);  // Without the dot, as written.
QString stem(const QString &path);
bool hasExtension(const QString &path, std::initializer_list<const char *> extensions);
QStringList ancestors(const QString &path);  // path, its parent, ..., the root.
int componentCount(const QString &path);
bool startsWith(const QString &path, const QString &prefix);  // Component-wise.
bool isAbsolute(const QString &path);
QString relative(const QString &path, const QString &base);  // Empty when path is not inside base.
}  // namespace path

struct FileInfo {
  bool exists = false, isFile = false, isDir = false, isSymlink = false;
  uint64_t size = 0, dev = 0, ino = 0;
  int64_t mtimeNs = 0;  // Since the Unix epoch.
  int64_t ctimeSec = 0, ctimeNsec = 0;
  std::optional<int64_t> birthSec;
  bool readOnly = false;
};
FileInfo stat(const QString &path);   // Follows symlinks.
FileInfo lstat(const QString &path);  // Describes the entry itself.
bool isFile(const QString &path);
bool isDir(const QString &path);
bool exists(const QString &path);  // Follows symlinks, like Path::exists.
bool entryExists(const QString &path);  // The directory entry, even a dangling symlink.
QString canonical(const QString &path);  // Throws when the path does not exist.
std::optional<QString> tryCanonical(const QString &path);
QString directoryId(const QString &path);  // Device and file id of a real directory.

QByteArray readAll(const QString &path, qint64 limit = -1);
void writeAll(const QString &path, const QByteArray &bytes);
void removeFile(const QString &path);
bool removeFileIfExists(const QString &path);
void removeTree(const QString &path);
void createDir(const QString &path);  // The parent must exist.
void createDirs(const QString &path);
void createDerivedDir(const QString &path);
QStringList listDir(const QString &path);  // Names, unsorted. Throws when unreadable.
struct DirEntry {
  QString name;
  bool isFile = false, isDir = false, isSymlink = false;
};
std::vector<DirEntry> readDir(const QString &path);

// A directory capability. Child lookups never follow links/reparse points. Keep this alive from admission through
// the last effect; checking a pathname and opening it again does not provide the same guarantee.
class Directory {
public:
  explicit Directory(const QString &absolutePath);
  ~Directory();
  Directory(Directory &&) noexcept;
  Directory &operator=(Directory &&) noexcept;
  Directory(const Directory &) = delete;
  Directory &operator=(const Directory &) = delete;
  Directory child(const QString &name) const;
  Directory childForRemoval(const QString &name) const;
  Directory ensureChild(const QString &name) const;
  Directory privateChild(const QString &name) const;  // Exclusive, owner-only staging; never reuses an entry.
  void requirePrivate() const;
  Directory descend(const QString &relativePath) const;
  QString identity() const;
  FileInfo selfInfo() const;
  Directory rebound(const Directory &parent, const QString &name) const;
  std::optional<Directory> optionalChild(const QString &name, bool forRemoval = false) const;
  std::vector<DirEntry> entries() const;
  QByteArray readRegular(const QString &name) const;
  std::optional<QByteArray> readOptionalRegular(const QString &name) const;
  // The QFile owns the open object; its deleter also retains the admitted parent capability.
  std::shared_ptr<QFile> openRegular(const QString &name, QIODevice::OpenMode mode = QIODevice::ReadOnly, bool mutation = false) const;
  FileInfo info(const QString &name, bool followLeaf = false) const;
  Directory share() const;
  intptr_t nativeHandle() const;
  void sync() const;
  bool renameTo(const QString &name, const Directory &destination, const QString &target, bool replace,
                QString *error = nullptr, QFile *openedSource = nullptr, const Directory *openedDirectory = nullptr) const;
  // Caller durably journals the intent first. A substituted entry is moved, inspected, and preserved on mismatch.
  // POSIX cannot rename/unlink by expected inode; private staging separates claim from later effects.
  void claim(const QString &name, const Directory &staging, const QString &target, const FileInfo &expected,
             QFile *openedSource = nullptr, const std::function<void()> &beforeRename = {}) const;
  void writeAtomic(const QString &name, const QByteArray &bytes,
                   const std::function<void(const QString &)> &beforePublication = {},
                   const std::function<void(const QString &)> &beforePrivatePublication = {}) const;
  bool removeFileIfPresent(const QString &name, const FileInfo *expected = nullptr, QFile *opened = nullptr) const;
  void removeEntry(const QString &name, const QString &expectedDirectoryId = {}, const Directory *admitted = nullptr) const;
  void removeEmpty(const QString &name, const Directory *admitted = nullptr) const;
private:
  friend class FileLock;
  struct Impl;
  void ensureCurrent() const;
  explicit Directory(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};
void syncDir(const QString &path);
void syncFile(const QString &path);

// Rename that fails when the target appeared concurrently.
bool renameNoClobber(const QString &source, const QString &target, QString *error = nullptr);
void renameReplace(const QString &source, const QString &target);
void atomicWrite(const QString &path, const QByteArray &bytes);  // Creates the parent.
void atomicWriteExisting(const QString &path, const QByteArray &bytes);  // The parent must exist.
void copyFile(const QString &source, const QString &target);  // Fails when the target exists.

class TempFile {
public:
  // A new empty file "<dir>/<prefix>XXXXXX<suffix>", removed on destruction unless persisted.
  TempFile(const QString &dir, const QString &prefix, const QString &suffix = {});
  ~TempFile();
  TempFile(const TempFile &) = delete;
  TempFile &operator=(const TempFile &) = delete;
  const QString &path() const { return path_; }
  void write(const QByteArray &bytes);
  void sync();
  void syncOrdered();  // See syncOpenedFileOrdered.
  qint64 size() const;
  void persist(const QString &target);  // Replaces the target.
  void persistNoClobber(const QString &target);
  void release() { path_.clear(); }
private:
  QString path_;
  QString boundPath_, stagingPath_;
  std::shared_ptr<QFile> file_;
  std::shared_ptr<Directory> directory_;
  FileAccess *access_ = nullptr;
};

class TempDir {
public:
  explicit TempDir(const QString &parent, const QString &prefix);
  ~TempDir();
  const QString &path() const { return path_; }
  void release() { path_.clear(); }
private:
  QString path_;
  std::shared_ptr<Directory> directory_;
  FileAccess *access_ = nullptr;
};

// Advisory lease on a file that stays in place. Closing the handle releases it, including on process death.
class FileLock {
public:
  ~FileLock();
  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;
  static std::unique_ptr<FileLock> tryAcquire(const QString &path);
  static std::unique_ptr<FileLock> tryAcquireShared(const QString &path);
  static std::unique_ptr<FileLock> acquire(const QString &path, std::chrono::milliseconds timeout, std::chrono::milliseconds poll);
  static std::unique_ptr<FileLock> acquireShared(const QString &path, std::chrono::milliseconds timeout, std::chrono::milliseconds poll);
  static std::unique_ptr<FileLock> acquireIn(const Directory &directory, const QString &name, std::chrono::milliseconds timeout, std::chrono::milliseconds poll);
  bool ownsPath(const QString &path) const;
private:
  FileLock() = default;
  static std::unique_ptr<FileLock> open(const QString &path);
  bool tryLock(bool shared);
  intptr_t handle_ = -1;
  std::shared_ptr<void> directory_;
};

void validateFileName(const QString &name);
bool validFileName(const QString &name);
QString sha256Hex(const QByteArray &bytes);
QString hexLower(uint64_t value);
QString toHex(const QByteArray &bytes);
std::optional<QByteArray> fromBase64(const QByteArray &text);  // Strict standard alphabet with padding.
}  // namespace core
