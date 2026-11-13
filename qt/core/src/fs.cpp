#include "fs.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#endif

namespace core {

namespace {
#ifndef Q_OS_WIN
QByteArray native(const QString &path) { return QFile::encodeName(path); }
QString errnoText(int error) { return QString::fromLocal8Bit(strerror(error)) + QStringLiteral(" (os error %1)").arg(error); }
#else
std::wstring native(const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); }
QString lastErrorText(DWORD error = GetLastError()) {
  wchar_t *buffer = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
  QString text = buffer ? QString::fromWCharArray(buffer).trimmed() : QStringLiteral("Windows error");
  if (buffer) LocalFree(buffer);
  return text + QStringLiteral(" (os error %1)").arg(error);
}
int64_t fileTimeNs(const FILETIME &time) {
  const int64_t ticks = (int64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  return (ticks - 116444736000000000LL) * 100;
}
#endif
QString randomName(int length = 6) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  QString name;
  for (int i = 0; i < length; ++i) name += QLatin1Char(alphabet[QRandomGenerator::global()->bounded(62)]);
  return name;
}
}  // namespace

namespace path {
bool isSeparator(QChar c) {
#ifdef Q_OS_WIN
  return c == QLatin1Char('\\') || c == QLatin1Char('/');
#else
  return c == QLatin1Char('/');
#endif
}
static int rootLength(const QString &p) {
#ifdef Q_OS_WIN
  if (p.startsWith(QLatin1String("\\\\?\\UNC\\"))) {
    int slashes = 0;
    for (int i = 8; i < p.size(); ++i)
      if (isSeparator(p[i]) && ++slashes == 2) return i + 1;
    return p.size();
  }
  if (p.startsWith(QLatin1String("\\\\?\\"))) {
    if (p.size() >= 6 && p[5] == QLatin1Char(':')) return p.size() > 6 && isSeparator(p[6]) ? 7 : 6;
    return 4;
  }
  if (p.size() >= 2 && isSeparator(p[0]) && isSeparator(p[1])) {
    int slashes = 0;
    for (int i = 2; i < p.size(); ++i)
      if (isSeparator(p[i]) && ++slashes == 2) return i + 1;
    return p.size();
  }
  if (p.size() >= 2 && p[1] == QLatin1Char(':')) return p.size() > 2 && isSeparator(p[2]) ? 3 : 2;
  return !p.isEmpty() && isSeparator(p[0]) ? 1 : 0;
#else
  return !p.isEmpty() && p[0] == QLatin1Char('/') ? 1 : 0;
#endif
}
static QString trimmed(const QString &p) {
  int end = p.size();
  const int root = rootLength(p);
  while (end > root && isSeparator(p[end - 1])) --end;
  QString out = p.left(end);
  // "a/." names "a" itself, as Path components do.
  while (out.size() > root + 1 && out.endsWith(QLatin1Char('.')) && isSeparator(out[out.size() - 2])) {
    out.chop(2);
    while (out.size() > root && isSeparator(out[out.size() - 1])) out.chop(1);
  }
  return out;
}
QString join(const QString &base, const QString &name) {
  if (base.isEmpty()) return name;
  if (isAbsolute(name)) return name;
  if (isSeparator(base.back())) return base + name;
  return base + kSeparator + name;
}
QString parent(const QString &p) {
  const QString t = trimmed(p);
  const int root = rootLength(t);
  if (t.size() <= root) return {};
  int i = t.size() - 1;
  while (i >= root && !isSeparator(t[i])) --i;
  if (i < root) return t.left(root);
  int end = i;
  while (end > root && isSeparator(t[end - 1])) --end;
  return t.left(qMax(end, root));
}
QString fileName(const QString &p) {
  const QString t = trimmed(p);
  const int root = rootLength(t);
  if (t.size() <= root) return {};
  int i = t.size() - 1;
  while (i >= root && !isSeparator(t[i])) --i;
  const QString name = t.mid(i + 1);
  return name == QLatin1String("..") ? QString() : name;
}
QString extension(const QString &p) {
  const QString name = fileName(p);
  const int dot = name.lastIndexOf(QLatin1Char('.'));
  if (dot <= 0) return {};
  return name.mid(dot + 1);
}
QString stem(const QString &p) {
  const QString name = fileName(p);
  const int dot = name.lastIndexOf(QLatin1Char('.'));
  return dot <= 0 ? name : name.left(dot);
}
bool hasExtension(const QString &p, std::initializer_list<const char *> extensions) {
  const QString ext = extension(p);
  if (ext.isEmpty()) return false;
  for (const char *candidate : extensions)
    if (ext.compare(QLatin1String(candidate), Qt::CaseInsensitive) == 0 && ext.toLower() == QLatin1String(candidate)) return true;
  return false;
}
QStringList ancestors(const QString &p) {
  QStringList out;
  QString current = trimmed(p);
  while (!current.isEmpty()) {
    out << current;
    const QString up = parent(current);
    if (up == current) break;
    current = up;
  }
  return out;
}
int componentCount(const QString &p) {
  const QString t = trimmed(p);
  const int root = rootLength(t);
  int count = root ? 1 : 0;
  bool inName = false;
  for (int i = root; i < t.size(); ++i) {
    if (isSeparator(t[i])) inName = false;
    else if (!inName) {
      inName = true;
      ++count;
    }
  }
  return count;
}
bool startsWith(const QString &p, const QString &prefix) {
  const QString a = trimmed(p), b = trimmed(prefix);
  if (!a.startsWith(b)) return false;
  return a.size() == b.size() || isSeparator(a[b.size()]) || (!b.isEmpty() && isSeparator(b.back()));
}
bool isAbsolute(const QString &p) {
#ifdef Q_OS_WIN
  const int root = rootLength(p);
  return root >= 3 || (root > 0 && p.startsWith(QLatin1String("\\\\")));
#else
  return p.startsWith(QLatin1Char('/'));
#endif
}
QString relative(const QString &p, const QString &base) {
  const QString a = trimmed(p), b = trimmed(base);
  if (!startsWith(a, b)) return {};
  QString rest = a.mid(b.size());
  while (!rest.isEmpty() && isSeparator(rest[0])) rest.remove(0, 1);
  return rest;
}
}  // namespace path

#ifndef Q_OS_WIN
static FileInfo fromStat(const struct stat &st) {
  FileInfo info;
  info.exists = true;
  info.isFile = S_ISREG(st.st_mode);
  info.isDir = S_ISDIR(st.st_mode);
  info.isSymlink = S_ISLNK(st.st_mode);
  info.size = uint64_t(st.st_size);
  info.dev = uint64_t(st.st_dev);
  info.ino = uint64_t(st.st_ino);
#ifdef Q_OS_MACOS
  info.mtimeNs = int64_t(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
  info.ctimeSec = st.st_ctimespec.tv_sec;
  info.ctimeNsec = st.st_ctimespec.tv_nsec;
  info.birthSec = st.st_birthtimespec.tv_sec;
#else
  info.mtimeNs = int64_t(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
  info.ctimeSec = st.st_ctim.tv_sec;
  info.ctimeNsec = st.st_ctim.tv_nsec;
#endif
  info.readOnly = !(st.st_mode & S_IWUSR);
  return info;
}
#ifdef Q_OS_LINUX
static void addBirth(const QString &p, bool follow, FileInfo &info) {
#if defined(STATX_BTIME) && defined(SYS_statx)
  struct statx sx {};
  if (syscall(SYS_statx, AT_FDCWD, native(p).constData(), follow ? 0 : AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) == 0 && (sx.stx_mask & STATX_BTIME))
    info.birthSec = sx.stx_btime.tv_sec;
#else
  Q_UNUSED(p) Q_UNUSED(follow) Q_UNUSED(info)
#endif
}
#endif
FileInfo stat(const QString &p) {
  struct stat st {};
  if (::stat(native(p).constData(), &st) != 0) return {};
  FileInfo info = fromStat(st);
#ifdef Q_OS_LINUX
  addBirth(p, true, info);
#endif
  return info;
}
FileInfo lstat(const QString &p) {
  struct stat st {};
  if (::lstat(native(p).constData(), &st) != 0) return {};
  FileInfo info = fromStat(st);
#ifdef Q_OS_LINUX
  addBirth(p, false, info);
#endif
  return info;
}
#else
static FileInfo statWindows(const QString &p, bool follow) {
  WIN32_FILE_ATTRIBUTE_DATA data {};
  if (!GetFileAttributesExW(native(p).c_str(), GetFileExInfoStandard, &data)) return {};
  FileInfo info;
  info.exists = true;
  const bool reparse = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
  if (reparse && follow) {
    HANDLE h = CreateFileW(native(p).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION by {};
    const bool ok = GetFileInformationByHandle(h, &by);
    CloseHandle(h);
    if (!ok) return {};
    data.dwFileAttributes = by.dwFileAttributes;
    data.ftCreationTime = by.ftCreationTime;
    data.ftLastWriteTime = by.ftLastWriteTime;
    data.nFileSizeHigh = by.nFileSizeHigh;
    data.nFileSizeLow = by.nFileSizeLow;
  }
  info.isSymlink = !follow && reparse;
  info.isDir = !info.isSymlink && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  info.isFile = !info.isSymlink && !info.isDir;
  info.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  info.mtimeNs = fileTimeNs(data.ftLastWriteTime);
  info.birthSec = fileTimeNs(data.ftCreationTime) / 1000000000LL;
  info.ctimeSec = info.mtimeNs / 1000000000LL;
  info.readOnly = data.dwFileAttributes & FILE_ATTRIBUTE_READONLY;
  return info;
}
FileInfo stat(const QString &p) { return statWindows(p, true); }
FileInfo lstat(const QString &p) { return statWindows(p, false); }
#endif

bool isFile(const QString &p) { return stat(p).isFile; }
bool isDir(const QString &p) { return stat(p).isDir; }
bool exists(const QString &p) { return stat(p).exists; }
bool entryExists(const QString &p) { return lstat(p).exists; }

std::optional<QString> tryCanonical(const QString &p) {
#ifndef Q_OS_WIN
  char *resolved = realpath(native(p).constData(), nullptr);
  if (!resolved) return std::nullopt;
  const QString out = QFile::decodeName(resolved);
  free(resolved);
  return out;
#else
  HANDLE h = CreateFileW(native(p).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::nullopt;
  std::wstring buffer(1024, L'\0');
  DWORD length = GetFinalPathNameByHandleW(h, buffer.data(), DWORD(buffer.size()), VOLUME_NAME_DOS);
  if (length >= buffer.size()) {
    buffer.resize(length + 1);
    length = GetFinalPathNameByHandleW(h, buffer.data(), DWORD(buffer.size()), VOLUME_NAME_DOS);
  }
  CloseHandle(h);
  if (!length) return std::nullopt;
  buffer.resize(length);
  return QString::fromStdWString(buffer);
#endif
}
QString canonical(const QString &p) {
  if (auto out = tryCanonical(p)) return *out;
#ifndef Q_OS_WIN
  fail(errnoText(errno));
#else
  fail(lastErrorText());
#endif
}

QString directoryId(const QString &p) {
  const FileInfo info = lstat(p);
  if (!info.exists) fail(QStringLiteral("No such file or directory (os error 2)"));
  if (!info.isDir) fail(QStringLiteral("Move album is not a regular directory"));
#ifndef Q_OS_WIN
  return QStringLiteral("%1-%2").arg(info.dev).arg(info.ino);
#else
  HANDLE h = CreateFileW(native(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail(lastErrorText());
  BY_HANDLE_FILE_INFORMATION by {};
  const bool ok = GetFileInformationByHandle(h, &by);
  CloseHandle(h);
  if (!ok) fail(lastErrorText());
  return QStringLiteral("%1-%2-%3").arg(by.dwVolumeSerialNumber).arg(by.nFileIndexHigh).arg(by.nFileIndexLow);
#endif
}

QByteArray readAll(const QString &p, qint64 limit) {
  QFile file(p);
  if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
  return limit < 0 ? file.readAll() : file.read(limit);
}
void writeAll(const QString &p, const QByteArray &bytes) {
  QFile file(p);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) fail(QStringLiteral("%1: %2").arg(p, file.errorString()));
  if (file.write(bytes) != bytes.size()) fail(file.errorString());
}
void removeFile(const QString &p) {
#ifndef Q_OS_WIN
  if (::unlink(native(p).constData()) != 0) fail(errnoText(errno));
#else
  if (!DeleteFileW(native(p).c_str())) fail(lastErrorText());
#endif
}
bool removeFileIfExists(const QString &p) {
#ifndef Q_OS_WIN
  if (::unlink(native(p).constData()) == 0) return true;
  if (errno == ENOENT) return false;
  fail(errnoText(errno));
#else
  if (DeleteFileW(native(p).c_str())) return true;
  const DWORD error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
  fail(lastErrorText(error));
#endif
}
void removeTree(const QString &p) {
  const FileInfo info = lstat(p);
  if (!info.exists) fail(QStringLiteral("No such file or directory (os error 2)"));
  if (!info.isDir) return removeFile(p);
  for (const DirEntry &entry : readDir(p)) {
    const QString child = path::join(p, entry.name);
    if (entry.isDir && !entry.isSymlink) removeTree(child);
    else removeFile(child);
  }
#ifndef Q_OS_WIN
  if (::rmdir(native(p).constData()) != 0) fail(errnoText(errno));
#else
  if (!RemoveDirectoryW(native(p).c_str())) fail(lastErrorText());
#endif
}
void createDir(const QString &p) {
#ifndef Q_OS_WIN
  if (::mkdir(native(p).constData(), 0777) != 0) {
    const int error = errno;
    if (error == EEXIST) fail(QStringLiteral("File exists (os error 17)"));
    fail(errnoText(error));
  }
#else
  if (!CreateDirectoryW(native(p).c_str(), nullptr)) {
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) fail(QStringLiteral("File exists (os error 17)"));
    fail(lastErrorText(error));
  }
#endif
}
void createDirs(const QString &p) {
  if (isDir(p)) return;
  if (!QDir().mkpath(p)) fail(QStringLiteral("Cannot create %1").arg(p));
}
void createDerivedDir(const QString &p) {
  try {
    createDir(p);
  } catch (const Error &error) {
    if (lstat(p).isDir) return;
    fail(QStringLiteral("%1: %2").arg(p, error.message()));
  }
}
std::vector<DirEntry> readDir(const QString &p) {
  std::vector<DirEntry> out;
#ifndef Q_OS_WIN
  DIR *dir = opendir(native(p).constData());
  if (!dir) fail(errnoText(errno));
  while (dirent *entry = readdir(dir)) {
    const char *name = entry->d_name;
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    DirEntry item;
    item.name = QFile::decodeName(name);
    unsigned char type = entry->d_type;
    if (type == DT_UNKNOWN) {
      struct stat st {};
      if (::lstat(native(path::join(p, item.name)).constData(), &st) == 0)
        type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
    }
    item.isDir = type == DT_DIR;
    item.isFile = type == DT_REG;
    item.isSymlink = type == DT_LNK;
    out.push_back(item);
  }
  closedir(dir);
#else
  WIN32_FIND_DATAW data {};
  HANDLE h = FindFirstFileW((native(p) + L"\\*").c_str(), &data);
  if (h == INVALID_HANDLE_VALUE) fail(lastErrorText());
  do {
    const QString name = QString::fromWCharArray(data.cFileName);
    if (name == QLatin1String(".") || name == QLatin1String("..")) continue;
    DirEntry item;
    item.name = name;
    item.isSymlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
    item.isDir = !item.isSymlink && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    item.isFile = !item.isSymlink && !item.isDir;
    out.push_back(item);
  } while (FindNextFileW(h, &data));
  FindClose(h);
#endif
  return out;
}
QStringList listDir(const QString &p) {
  QStringList names;
  for (const DirEntry &entry : readDir(p)) names << entry.name;
  return names;
}
void syncDir(const QString &p) {
#ifndef Q_OS_WIN
  const int fd = ::open(native(p).constData(), O_RDONLY);
  if (fd < 0) fail(errnoText(errno));
  const int result = ::fsync(fd);
  const int error = errno;
  ::close(fd);
  if (result != 0) fail(errnoText(error));
#else
  Q_UNUSED(p)
#endif
}
void syncFile(const QString &p) {
#ifndef Q_OS_WIN
  const int fd = ::open(native(p).constData(), O_RDONLY);
  if (fd < 0) fail(errnoText(errno));
#ifdef Q_OS_MACOS
  const int result = ::fcntl(fd, F_FULLFSYNC) == 0 ? 0 : ::fsync(fd);
#else
  const int result = ::fsync(fd);
#endif
  const int error = errno;
  ::close(fd);
  if (result != 0) fail(errnoText(error));
#else
  HANDLE h = CreateFileW(native(p).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail(lastErrorText());
  const bool ok = FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok) fail(lastErrorText());
#endif
}

bool renameNoClobber(const QString &source, const QString &target, QString *error) {
#if defined(Q_OS_MACOS)
  if (renamex_np(native(source).constData(), native(target).constData(), RENAME_EXCL) == 0) return true;
  if (error) *error = errnoText(errno);
  return false;
#elif defined(Q_OS_LINUX)
  if (syscall(SYS_renameat2, AT_FDCWD, native(source).constData(), AT_FDCWD, native(target).constData(), 1 /* RENAME_NOREPLACE */) == 0) return true;
  if (error) *error = errnoText(errno);
  return false;
#else
  if (MoveFileW(native(source).c_str(), native(target).c_str())) return true;
  if (error) *error = lastErrorText();
  return false;
#endif
}
void renameReplace(const QString &source, const QString &target) {
#ifndef Q_OS_WIN
  if (::rename(native(source).constData(), native(target).constData()) != 0) fail(errnoText(errno));
#else
  if (!MoveFileExW(native(source).c_str(), native(target).c_str(), MOVEFILE_REPLACE_EXISTING)) fail(lastErrorText());
#endif
}

void atomicWrite(const QString &p, const QByteArray &bytes) {
  const QString dir = path::parent(p);
  if (dir.isEmpty()) fail(QStringLiteral("Missing parent directory"));
  createDirs(dir);
  atomicWriteExisting(p, bytes);
}
void atomicWriteExisting(const QString &p, const QByteArray &bytes) {
  const QString dir = path::parent(p);
  if (dir.isEmpty()) fail(QStringLiteral("Missing parent directory"));
  TempFile temp(dir, QStringLiteral(".tmp"));
  temp.write(bytes);
  temp.sync();
  temp.persist(p);
}
void copyFile(const QString &source, const QString &target) {
#ifdef Q_OS_WIN
  if (!CopyFileW(native(source).c_str(), native(target).c_str(), TRUE)) fail(lastErrorText());
#else
  if (entryExists(target)) fail(QStringLiteral("File exists (os error 17)"));
  QFile in(source);
  if (!in.open(QIODevice::ReadOnly)) fail(in.errorString());
  const int fd = ::open(native(target).constData(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) fail(errnoText(errno));
  QFile out;
  if (!out.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
    ::close(fd);
    fail(out.errorString());
  }
  QByteArray chunk;
  while (!(chunk = in.read(1 << 20)).isEmpty())
    if (out.write(chunk) != chunk.size()) fail(out.errorString());
  out.close();
  struct stat st {};
  if (::stat(native(source).constData(), &st) == 0) ::chmod(native(target).constData(), st.st_mode & 07777);
#endif
}

TempFile::TempFile(const QString &dir, const QString &prefix, const QString &suffix) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    const QString candidate = path::join(dir, prefix + randomName() + suffix);
#ifndef Q_OS_WIN
    const int fd = ::open(native(candidate).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
      ::close(fd);
      path_ = candidate;
      return;
    }
    if (errno != EEXIST) fail(errnoText(errno));
#else
    HANDLE h = CreateFileW(native(candidate).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
      path_ = candidate;
      return;
    }
    if (GetLastError() != ERROR_FILE_EXISTS) fail(lastErrorText());
#endif
  }
  fail(QStringLiteral("Cannot create a temporary file in %1").arg(dir));
}
TempFile::~TempFile() {
  if (!path_.isEmpty()) {
    try {
      removeFileIfExists(path_);
    } catch (...) {
    }
  }
}
void TempFile::write(const QByteArray &bytes) {
  QFile file(path_);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) fail(file.errorString());
  if (file.write(bytes) != bytes.size()) fail(file.errorString());
}
void TempFile::sync() { syncFile(path_); }
qint64 TempFile::size() const { return qint64(stat(path_).size); }
void TempFile::persist(const QString &target) {
  renameReplace(path_, target);
  path_.clear();
}
void TempFile::persistNoClobber(const QString &target) {
  QString error;
  if (!renameNoClobber(path_, target, &error)) fail(error);
  path_.clear();
}

TempDir::TempDir(const QString &parent, const QString &prefix) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    const QString candidate = path::join(parent, prefix + randomName());
    try {
      createDir(candidate);
      path_ = candidate;
      return;
    } catch (const Error &) {
      if (!entryExists(candidate)) throw;
    }
  }
  fail(QStringLiteral("Cannot create a temporary directory in %1").arg(parent));
}
TempDir::~TempDir() {
  if (!path_.isEmpty()) {
    try {
      removeTree(path_);
    } catch (...) {
    }
  }
}

FileLock::~FileLock() {
#ifndef Q_OS_WIN
  if (handle_ >= 0) {
    while (flock(int(handle_), LOCK_UN) != 0 && errno == EINTR) {
    }
    ::close(int(handle_));
  }
#else
  if (handle_ != -1) {
    OVERLAPPED overlapped {};
    UnlockFileEx(HANDLE(handle_), 0, MAXDWORD, MAXDWORD, &overlapped);
    CloseHandle(HANDLE(handle_));
  }
#endif
}
std::unique_ptr<FileLock> FileLock::open(const QString &p) {
  if (lstat(p).isSymlink) fail(QStringLiteral("File lease cannot follow a symlink"));
  std::unique_ptr<FileLock> lock(new FileLock);
#ifndef Q_OS_WIN
  const int fd = ::open(native(p).constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) fail(QStringLiteral("Cannot open lock %1: %2").arg(p, errnoText(errno)));
  lock->handle_ = fd;
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) fail(QStringLiteral("Lock is not a regular file: %1").arg(p));
#else
  HANDLE h = CreateFileW(native(p).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail(QStringLiteral("Cannot open lock %1: %2").arg(p, lastErrorText()));
  lock->handle_ = intptr_t(h);
  if (GetFileType(h) != FILE_TYPE_DISK) fail(QStringLiteral("Lock is not a regular file: %1").arg(p));
#endif
  return lock;
}
bool FileLock::tryLock(bool shared) {
#ifndef Q_OS_WIN
  for (;;) {
    if (flock(int(handle_), (shared ? LOCK_SH : LOCK_EX) | LOCK_NB) == 0) return true;
    if (errno == EINTR) continue;
    if (errno == EWOULDBLOCK) return false;
    fail(errnoText(errno));
  }
#else
  OVERLAPPED overlapped {};
  if (LockFileEx(HANDLE(handle_), (shared ? 0 : LOCKFILE_EXCLUSIVE_LOCK) | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped)) return true;
  const DWORD error = GetLastError();
  if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) return false;
  fail(lastErrorText(error));
#endif
}
std::unique_ptr<FileLock> FileLock::tryAcquire(const QString &p) {
  auto lock = open(p);
  return lock->tryLock(false) ? std::move(lock) : nullptr;
}
std::unique_ptr<FileLock> FileLock::tryAcquireShared(const QString &p) {
  auto lock = open(p);
  return lock->tryLock(true) ? std::move(lock) : nullptr;
}
static std::unique_ptr<FileLock> waitFor(const QString &p, std::chrono::milliseconds timeout, std::chrono::milliseconds poll, bool shared) {
  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    auto lock = shared ? FileLock::tryAcquireShared(p) : FileLock::tryAcquire(p);
    if (lock) return lock;
    if (std::chrono::steady_clock::now() - started >= timeout) fail(QStringLiteral("File lock timed out: %1").arg(p));
    std::this_thread::sleep_for(std::max(poll, std::chrono::milliseconds(1)));
  }
}
std::unique_ptr<FileLock> FileLock::acquire(const QString &p, std::chrono::milliseconds timeout, std::chrono::milliseconds poll) { return waitFor(p, timeout, poll, false); }
std::unique_ptr<FileLock> FileLock::acquireShared(const QString &p, std::chrono::milliseconds timeout, std::chrono::milliseconds poll) { return waitFor(p, timeout, poll, true); }
bool FileLock::ownsPath(const QString &p) const {
#ifndef Q_OS_WIN
  struct stat current {}, owned {};
  if (::lstat(native(p).constData(), &current) != 0 || fstat(int(handle_), &owned) != 0) return false;
  return S_ISREG(current.st_mode) && current.st_dev == owned.st_dev && current.st_ino == owned.st_ino;
#else
  const FileInfo current = lstat(p);
  if (!current.isFile) return false;
  FILETIME created {}, accessed {}, written {};
  if (!GetFileTime(HANDLE(handle_), &created, &accessed, &written)) return false;
  return current.birthSec && *current.birthSec == fileTimeNs(created) / 1000000000LL;
#endif
}

bool validFileName(const QString &name) {
  if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) return false;
  for (QChar c : name)
    if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') || c == QChar(0)) return false;
  return true;
}
void validateFileName(const QString &name) {
  if (!validFileName(name)) fail(QStringLiteral("Expected a single file name"));
}
std::optional<QByteArray> fromBase64(const QByteArray &text) {
  auto result = QByteArray::fromBase64Encoding(text, QByteArray::AbortOnBase64DecodingErrors);
  if (!result) return std::nullopt;
  return result.decoded;
}
QString toHex(const QByteArray &bytes) { return QString::fromLatin1(bytes.toHex()); }
QString sha256Hex(const QByteArray &bytes) { return toHex(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)); }
QString hexLower(uint64_t value) { return QString::number(qulonglong(value), 16); }

}  // namespace core
