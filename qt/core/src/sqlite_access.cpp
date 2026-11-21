#include "sqlite_access.h"
#include "file_access.h"
#include <sqlite3.h>
#include <mutex>

#ifndef Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace core::db {
namespace {
// Keep SQLite's native unix VFS, file locking, hot-journal recovery and fsync semantics. Its documented syscall
// override interface supplies dirfd-relative name resolution only while a capability-backed connection is active.
sqlite3_vfs *unixVfs = nullptr;
int (*originalOpen)(const char *, int, int);
int (*originalStat)(const char *, struct stat *);
int (*originalLstat)(const char *, struct stat *);
int (*originalAccess)(const char *, int);
int (*originalUnlink)(const char *);
int (*originalOpenDirectory)(const char *, int *);
int (*originalMkdir)(const char *, mode_t);
int (*originalRmdir)(const char *);
ssize_t (*originalReadlink)(const char *, char *, size_t);

template <typename Work> int safe(Work work) noexcept {
  try { return work(); } catch (...) { errno = EACCES; return -1; }
}
int boundOpen(const char *name, int flags, int mode) {
  const QString path = QFile::decodeName(name);
  auto *view = FileAccess::current(path);
  if (!view) return originalOpen(name, flags, mode);
  return safe([&] {
    // Never let O_TRUNC act on an object before checking its type. Native SQLite normally doesn't truncate at open.
    auto directory = view->directory(core::path::parent(path));
    view->observeDirectory(core::path::parent(path));
    const auto leaf = QFile::encodeName(core::path::fileName(path));
    const int fd = openat(int(directory->nativeHandle()), leaf.constData(), (flags & ~O_TRUNC) | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !(S_ISREG(st.st_mode) || ((flags & O_DIRECTORY) && S_ISDIR(st.st_mode)))) {
      ::close(fd); errno = EACCES; return -1;
    }
    if ((flags & O_TRUNC) && ftruncate(fd, 0) != 0) { const int code = errno; ::close(fd); errno = code; return -1; }
    return fd;
  });
}
int boundStat(const char *name, struct stat *out) {
  const QString path = QFile::decodeName(name);
  auto *view = FileAccess::current(path);
  if (!view) return originalStat(name, out);
  return safe([&] {
    if (view->info(path).isDir) return fstat(int(view->directory(path)->nativeHandle()), out);
    return fstatat(int(view->directory(core::path::parent(path))->nativeHandle()), QFile::encodeName(core::path::fileName(path)).constData(), out, AT_SYMLINK_NOFOLLOW);
  });
}
int boundLstat(const char *name, struct stat *out) {
  if (!FileAccess::current(QFile::decodeName(name))) return originalLstat(name, out);
  return boundStat(name, out);
}
int boundAccess(const char *name, int mode) {
  const QString path = QFile::decodeName(name);
  auto *view = FileAccess::current(path);
  if (!view) return originalAccess(name, mode);
  return safe([&] {
    auto directory = view->directory(core::path::parent(path));
    const auto info = directory->info(core::path::fileName(path));
    if (!info.exists || info.isSymlink) { errno = info.exists ? EACCES : ENOENT; return -1; }
    return faccessat(int(directory->nativeHandle()), QFile::encodeName(core::path::fileName(path)).constData(), mode, 0);
  });
}
int boundUnlink(const char *name) {
  const QString path = QFile::decodeName(name);
  auto *view = FileAccess::current(path);
  if (!view) return originalUnlink(name);
  return safe([&] {
    return unlinkat(int(view->directory(core::path::parent(path))->nativeHandle()), QFile::encodeName(core::path::fileName(path)).constData(), 0);
  });
}
int boundOpenDirectory(const char *name, int *out) {
  const QString path = QFile::decodeName(name);
  auto *view = FileAccess::current(path);
  if (!view) return originalOpenDirectory(name, out);
  return safe([&] {
    *out = fcntl(int(view->directory(core::path::parent(path))->nativeHandle()), F_DUPFD_CLOEXEC, 3);
    return *out < 0 ? -1 : 0;
  });
}
int boundMkdir(const char *name, mode_t mode) {
  const QString path = QFile::decodeName(name);
  auto *view = FileAccess::current(path);
  if (!view) return originalMkdir(name, mode);
  return safe([&] { return mkdirat(int(view->directory(core::path::parent(path))->nativeHandle()), QFile::encodeName(core::path::fileName(path)).constData(), mode); });
}
int boundRmdir(const char *name) {
  const QString path = QFile::decodeName(name);
  auto *view = FileAccess::current(path);
  if (!view) return originalRmdir(name);
  return safe([&] { return unlinkat(int(view->directory(core::path::parent(path))->nativeHandle()), QFile::encodeName(core::path::fileName(path)).constData(), AT_REMOVEDIR); });
}
ssize_t boundReadlink(const char *name, char *out, size_t size) {
  if (!FileAccess::current(QFile::decodeName(name))) return originalReadlink(name, out, size);
  errno = EACCES; return -1;
}
template <typename Callback> void replace(const char *name, Callback callback, Callback &original) {
  original = reinterpret_cast<Callback>(unixVfs->xGetSystemCall(unixVfs, name));
  if (!original || unixVfs->xSetSystemCall(unixVfs, name, reinterpret_cast<sqlite3_syscall_ptr>(callback)) != SQLITE_OK)
    fail(QStringLiteral("SQLite cannot bind filesystem operations to directory capabilities"));
}
}  // namespace
void initializeFilesystemBridge() {
  static std::once_flag once;
  std::call_once(once, [] {
    unixVfs = sqlite3_vfs_find("unix");
    if (!unixVfs || unixVfs->iVersion < 3 || !unixVfs->xSetSystemCall || !unixVfs->xGetSystemCall)
      fail(QStringLiteral("SQLite native filesystem bridge is unavailable"));
    replace("open", boundOpen, originalOpen); replace("stat", boundStat, originalStat); replace("lstat", boundLstat, originalLstat);
    replace("access", boundAccess, originalAccess); replace("unlink", boundUnlink, originalUnlink);
    replace("openDirectory", boundOpenDirectory, originalOpenDirectory); replace("mkdir", boundMkdir, originalMkdir);
    replace("rmdir", boundRmdir, originalRmdir); replace("readlink", boundReadlink, originalReadlink);
  });
}
}  // namespace core::db
#else
#include <windows.h>
namespace core::db {
namespace {
using Create = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
Create originalCreate = nullptr;
HANDLE WINAPI boundCreate(LPCWSTR name, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE templateFile) {
  const QString path = QString::fromWCharArray(name);
  auto *view = FileAccess::current(path);
  if (!view) return originalCreate(name, access, sharing, security, creation, flags, templateFile);
  try {
    // SQLite keeps its native Windows VFS, handles, locks and journal protocol. Guard every leaf it opens, including
    // rollback journals and WAL/shm sidecars, rather than guarding only the main database pathname.
    auto parent = view->directory(core::path::parent(path));
    view->observeDirectory(core::path::parent(path));
    const bool truncate = creation == CREATE_ALWAYS || creation == TRUNCATE_EXISTING;
    const bool removeOnClose = flags & FILE_FLAG_DELETE_ON_CLOSE;
    const DWORD safeCreation = creation == CREATE_ALWAYS ? OPEN_ALWAYS : creation == TRUNCATE_EXISTING ? OPEN_EXISTING : creation;
    HANDLE file = originalCreate(name, access | (removeOnClose ? DELETE : 0), sharing, security, safeCreation,
        (flags & ~FILE_FLAG_DELETE_ON_CLOSE) | FILE_FLAG_OPEN_REPARSE_POINT, templateFile);
    if (file == INVALID_HANDLE_VALUE) return file;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || GetFileType(file) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
      CloseHandle(file); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
    }
    if (truncate) {
      LARGE_INTEGER zero{};
      if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
        const DWORD error = GetLastError(); CloseHandle(file); SetLastError(error); return INVALID_HANDLE_VALUE;
      }
    }
    if (removeOnClose) {
      FILE_DISPOSITION_INFO disposition{TRUE};
      if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
        const DWORD error = GetLastError(); CloseHandle(file); SetLastError(error); return INVALID_HANDLE_VALUE;
      }
    }
    return file;
  } catch (...) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
}
}
void initializeFilesystemBridge() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto *vfs = sqlite3_vfs_find("win32");
    if (!vfs || vfs->iVersion < 3 || !vfs->xGetSystemCall || !vfs->xSetSystemCall)
      fail(QStringLiteral("SQLite Windows filesystem bridge is unavailable"));
    originalCreate = reinterpret_cast<Create>(vfs->xGetSystemCall(vfs, "CreateFileW"));
    if (!originalCreate || vfs->xSetSystemCall(vfs, "CreateFileW", reinterpret_cast<sqlite3_syscall_ptr>(boundCreate)) != SQLITE_OK)
      fail(QStringLiteral("SQLite cannot bind Windows file opens to admitted objects"));
  });
}
}
#endif
