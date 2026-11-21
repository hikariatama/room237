#include "fs.h"
#include "file_access.h"

#include <QDir>
#include <QFile>
#include <QUuid>
#include <QScopeGuard>
#include <algorithm>
#include <cstring>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#include <fcntl.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#endif
#ifdef Q_OS_MACOS
#include <sys/acl.h>
#endif
#endif

namespace core {
namespace {
[[noreturn]] void ioError(const QString &operation) {
#ifdef Q_OS_WIN
  fail(QStringLiteral("%1 (Windows error %2)").arg(operation).arg(GetLastError()));
#else
  fail(operation + QStringLiteral(": ") + QString::fromLocal8Bit(strerror(errno)));
#endif
}
void leaf(const QString &name) {
  if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) fail(QStringLiteral("Expected a native directory entry name"));
  for (QChar c : name)
    if (c == QChar(0) || path::isSeparator(c)
#ifdef Q_OS_WIN
        || c == QLatin1Char(':')
#endif
    ) fail(QStringLiteral("Expected a native directory entry name"));
}
#ifdef Q_OS_WIN
std::vector<char> currentUser() {
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) ioError(QStringLiteral("Read staging owner"));
  auto close = qScopeGuard([&] { CloseHandle(token); });
  DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<char> user(size);
  if (!GetTokenInformation(token, TokenUser, user.data(), size, &size)) ioError(QStringLiteral("Read staging owner"));
  return user;
}
#endif
}  // namespace

struct Directory::Impl {
  std::shared_ptr<Impl> parent;
  QString name;
#ifdef Q_OS_WIN
  HANDLE handle = INVALID_HANDLE_VALUE;
  QString path;
  bool removal = false;
  // Windows pathname lookups are safe only while every ancestor is held without write/delete sharing. Reject all
  // reparse points (including junctions), not just IO_REPARSE_TAG_SYMLINK. File publication/deletion uses handles.
  ~Impl() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
  int fd = -1;
  ~Impl() { if (fd >= 0) ::close(fd); }
#endif
};

Directory::Directory(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Directory::~Directory() = default;
Directory::Directory(Directory &&) noexcept = default;
Directory &Directory::operator=(Directory &&) noexcept = default;

Directory::Directory(const QString &absolutePath) {
  if (!path::isAbsolute(absolutePath)) fail(QStringLiteral("A directory capability needs an absolute path"));
  QString admittedPath = absolutePath;
#ifdef Q_OS_MACOS
  // These root-owned aliases are part of the macOS filesystem layout. Resolve only their documented target,
  // before walking with O_NOFOLLOW; never canonicalize arbitrary components supplied by the library.
  for (const QString &alias : {QStringLiteral("/tmp"), QStringLiteral("/var")}) {
    if (!path::startsWith(admittedPath, alias)) continue;
    struct stat st{};
    const QByteArray encoded = QFile::encodeName(alias);
    char target[64];
    const ssize_t count = ::readlink(encoded.constData(), target, sizeof(target));
    const QByteArray expected = QFile::encodeName(QStringLiteral("private") + alias);
    if (::lstat(encoded.constData(), &st) == 0 && S_ISLNK(st.st_mode) && st.st_uid == 0 &&
        count == expected.size() && QByteArray(target, count) == expected)
      admittedPath = QStringLiteral("/private") + admittedPath;
  }
#endif
  const QStringList ancestors = path::ancestors(admittedPath);
  if (ancestors.isEmpty()) fail(QStringLiteral("Missing directory"));
  impl_ = std::make_shared<Impl>();
#ifdef Q_OS_WIN
  impl_->path = ancestors.back();
  impl_->handle = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(impl_->path).utf16()),
      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (impl_->handle == INVALID_HANDLE_VALUE) ioError(QStringLiteral("Open directory root"));
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(impl_->handle, &info)) ioError(QStringLiteral("Inspect directory root"));
  if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    fail(QStringLiteral("Directory root is not a regular directory"));
#else
  impl_->fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (impl_->fd < 0) ioError(QStringLiteral("Open directory root"));
#endif
  for (auto it = ancestors.rbegin() + 1; it != ancestors.rend(); ++it) {
    *this = child(path::fileName(*it));
#ifndef Q_OS_WIN
    // Keep the admitted root and its immediate parent, with bounded descriptors even for a deep absolute prefix.
    // Later effects never re-resolve that discarded prefix. Descendants retain the chain below this root.
    if (impl_->parent) impl_->parent->parent.reset();
#endif
  }
}

std::optional<Directory> Directory::optionalChild(const QString &name, bool forRemoval) const {
  leaf(name);
  ensureCurrent();
  auto next = std::make_shared<Impl>();
  next->parent = impl_;
  next->name = name;
#ifdef Q_OS_WIN
  next->path = path::join(impl_->path, name);
  next->removal = forRemoval;
  next->handle = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(next->path).utf16()),
      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | (forRemoval ? DELETE | READ_CONTROL : 0), FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (next->handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return std::nullopt;
    ioError(QStringLiteral("Open child directory"));
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(next->handle, &info)) ioError(QStringLiteral("Inspect child directory"));
  if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    fail(QStringLiteral("Service directory is not a regular directory"));
#else
  Q_UNUSED(forRemoval)
  next->fd = ::openat(impl_->fd, QFile::encodeName(name).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (next->fd < 0) {
    if (errno == ENOENT) return std::nullopt;
    ioError(QStringLiteral("Open child directory without following links"));
  }
#endif
  return Directory(std::move(next));
}
Directory Directory::child(const QString &name) const {
  auto result = optionalChild(name);
  if (!result) fail(QStringLiteral("Missing child directory: %1").arg(name));
  return std::move(*result);
}
Directory Directory::childForRemoval(const QString &name) const {
  auto result = optionalChild(name, true);
  if (!result) fail(QStringLiteral("Missing child directory: %1").arg(name));
  return std::move(*result);
}
Directory Directory::ensureChild(const QString &name) const {
  leaf(name);
  ensureCurrent();
#ifdef Q_OS_WIN
  const auto wide = QDir::toNativeSeparators(path::join(impl_->path, name)).toStdWString();
  if (!CreateDirectoryW(wide.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) ioError(QStringLiteral("Create service directory"));
#else
  if (::mkdirat(impl_->fd, QFile::encodeName(name).constData(), 0777) != 0 && errno != EEXIST) ioError(QStringLiteral("Create service directory"));
#endif
  return child(name);
}
Directory Directory::privateChild(const QString &name) const {
  leaf(name); ensureCurrent();
#ifdef Q_OS_WIN
  const auto user = currentUser();
  PSID sid = reinterpret_cast<const TOKEN_USER *>(user.data())->User.Sid;
  std::vector<char> storage(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(sid));
  auto *acl = reinterpret_cast<ACL *>(storage.data());
  SECURITY_DESCRIPTOR descriptor;
  if (!InitializeAcl(acl, DWORD(storage.size()), ACL_REVISION) ||
      !AddAccessAllowedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, FILE_ALL_ACCESS, sid) ||
      !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorOwner(&descriptor, sid, FALSE) || !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
      !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) ioError(QStringLiteral("Protect exclusive staging"));
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
  const auto wide = QDir::toNativeSeparators(path::join(impl_->path, name)).toStdWString();
  if (!CreateDirectoryW(wide.c_str(), &security)) ioError(QStringLiteral("Create exclusive staging directory"));
  auto directory = childForRemoval(name);
  directory.requirePrivate();
  return directory;
#else
  if (::mkdirat(impl_->fd, QFile::encodeName(name).constData(), 0700) != 0) ioError(QStringLiteral("Create exclusive staging directory"));
  auto directory = child(name);
  struct stat st{};
  if (fstat(int(directory.nativeHandle()), &st) != 0 || st.st_uid != geteuid())
    fail(QStringLiteral("Staging ownership changed; preserving it"));
#ifdef Q_OS_MACOS
  // macOS inherited ACLs can grant access independently of mode bits. A new private staging directory has no grants.
  acl_t empty = acl_init(0);
  if (!empty) ioError(QStringLiteral("Create private staging ACL"));
  const int result = acl_set_fd_np(int(directory.nativeHandle()), empty, ACL_TYPE_EXTENDED);
  acl_free(empty);
  if (result != 0) ioError(QStringLiteral("Remove inherited staging grants"));
#endif
  if (fchmod(int(directory.nativeHandle()), 0700) != 0) ioError(QStringLiteral("Protect staging directory"));
  directory.requirePrivate();
  return directory;
#endif
}
void Directory::requirePrivate() const {
  ensureCurrent();
#ifdef Q_OS_WIN
  PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD result = GetSecurityInfo(impl_->handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &acl, nullptr, &descriptor);
  if (result != ERROR_SUCCESS) { SetLastError(result); ioError(QStringLiteral("Inspect staging protection")); }
  auto release = qScopeGuard([&] { LocalFree(descriptor); });
  const auto user = currentUser();
  PSID sid = reinterpret_cast<const TOKEN_USER *>(user.data())->User.Sid;
  SECURITY_DESCRIPTOR_CONTROL control; DWORD revision;
  if (!owner || !EqualSid(owner, sid) || !acl || !GetSecurityDescriptorControl(descriptor, &control, &revision) || !(control & SE_DACL_PROTECTED))
    fail(QStringLiteral("Staging is not protected for the current owner; preserving it"));
  bool allowed = false;
  for (DWORD i = 0; i < acl->AceCount; ++i) {
    void *raw = nullptr;
    if (!GetAce(acl, i, &raw)) ioError(QStringLiteral("Inspect staging ACL"));
    const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || !EqualSid(const_cast<DWORD *>(&ace->SidStart), sid))
      fail(QStringLiteral("Staging grants access to another principal; preserving it"));
    allowed |= (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
  }
  if (!allowed) fail(QStringLiteral("Staging owner lacks required access; preserving it"));
#else
  struct stat st{};
  if (fstat(impl_->fd, &st) != 0) ioError(QStringLiteral("Inspect staging protection"));
  if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) fail(QStringLiteral("Staging is not private to the current owner; preserving it"));
#ifdef Q_OS_MACOS
  // acl_get_fd_np conflates a missing ACL property with retrieval errors (NULL/ENOENT on a fresh APFS object).
  // Query the public filesec property after an explicitly successful descriptor-based fstatx instead.
  filesec_t security = filesec_init();
  if (!security) ioError(QStringLiteral("Inspect staging ACL"));
  auto release = qScopeGuard([&] { filesec_free(security); });
  struct stat secured{};
  int present = 0;
  if (fstatx_np(impl_->fd, &secured, security) != 0 || filesec_query_property(security, FILESEC_ACL, &present) != 0)
    ioError(QStringLiteral("Inspect staging ACL"));
  if (!S_ISDIR(secured.st_mode) || secured.st_uid != geteuid() || (secured.st_mode & 0077)) fail(QStringLiteral("Staging protection changed"));
  if (!present) return;
  acl_t acl = nullptr;
  if (filesec_get_property(security, FILESEC_ACL, &acl) != 0 || !acl) ioError(QStringLiteral("Retrieve staging ACL"));
  acl_entry_t entry;
  const int count = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
  acl_free(acl);
  if (count != 0) fail(QStringLiteral("Staging has additional ACL grants; preserving it"));
#endif
#endif
}
Directory Directory::descend(const QString &relativePath) const {
  if (path::isAbsolute(relativePath)) fail(QStringLiteral("Expected a relative directory path"));
  Directory result(impl_);
  const QString normalized = QDir::fromNativeSeparators(relativePath);
  if (normalized.isEmpty()) return result;
  for (const QString &name : normalized.split(QLatin1Char('/'))) result = result.child(name);
  return result;
}
Directory Directory::share() const { return Directory(impl_); }
intptr_t Directory::nativeHandle() const {
  ensureCurrent();
#ifdef Q_OS_WIN
  return intptr_t(impl_->handle);
#else
  return impl_->fd;
#endif
}
void Directory::sync() const {
  ensureCurrent();
#ifndef Q_OS_WIN
  if (::fsync(impl_->fd) != 0) ioError(QStringLiteral("Sync pinned directory"));
#endif
}

FileInfo Directory::info(const QString &name, bool followLeaf) const {
  leaf(name);
  ensureCurrent();
#ifdef Q_OS_WIN
  const QString full = path::join(impl_->path, name);
  HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(full).utf16()),
      FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      (followLeaf ? 0 : FILE_FLAG_OPEN_REPARSE_POINT) | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return {};
    ioError(QStringLiteral("Inspect pinned entry"));
  }
  BY_HANDLE_FILE_INFORMATION st{};
  const bool ok = GetFileInformationByHandle(handle, &st);
  CloseHandle(handle);
  if (!ok) ioError(QStringLiteral("Inspect pinned entry"));
  auto ns = [](const FILETIME &t) -> qint64 { return (((qint64(t.dwHighDateTime) << 32) | t.dwLowDateTime) - 116444736000000000LL) * 100; };
  FileInfo out;
  out.exists = true; out.isSymlink = st.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
  out.isDir = !out.isSymlink && (st.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY); out.isFile = !out.isSymlink && !out.isDir;
  out.dev = st.dwVolumeSerialNumber; out.ino = (quint64(st.nFileIndexHigh) << 32) | st.nFileIndexLow;
  out.size = (quint64(st.nFileSizeHigh) << 32) | st.nFileSizeLow; out.mtimeNs = ns(st.ftLastWriteTime);
  out.birthSec = ns(st.ftCreationTime) / 1000000000; out.ctimeSec = out.mtimeNs / 1000000000;
  out.readOnly = st.dwFileAttributes & FILE_ATTRIBUTE_READONLY;
  return out;
#else
  struct stat st{};
  if (::fstatat(impl_->fd, QFile::encodeName(name).constData(), &st, followLeaf ? 0 : AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) return {};
    ioError(QStringLiteral("Inspect pinned entry"));
  }
  FileInfo out;
  out.exists = true; out.isFile = S_ISREG(st.st_mode); out.isDir = S_ISDIR(st.st_mode); out.isSymlink = S_ISLNK(st.st_mode);
  out.size = st.st_size; out.dev = st.st_dev; out.ino = st.st_ino; out.readOnly = !(st.st_mode & S_IWUSR);
#ifdef Q_OS_MACOS
  out.mtimeNs = qint64(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
  out.ctimeSec = st.st_ctimespec.tv_sec; out.ctimeNsec = st.st_ctimespec.tv_nsec; out.birthSec = st.st_birthtimespec.tv_sec;
#else
  out.mtimeNs = qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
  out.ctimeSec = st.st_ctim.tv_sec; out.ctimeNsec = st.st_ctim.tv_nsec;
#endif
  return out;
#endif
}

std::shared_ptr<QFile> Directory::openRegular(const QString &name, QIODevice::OpenMode mode, bool mutation) const {
  ensureCurrent();
  leaf(name);
  const bool writing = mode & QIODevice::WriteOnly;
  const bool reading = mode & QIODevice::ReadOnly;
  // Truncate only after the opened object has been inspected. O_NONBLOCK prevents a FIFO substitution hanging open.
#ifdef Q_OS_WIN
  DWORD creation = !writing || (mode & QIODevice::ExistingOnly) ? OPEN_EXISTING : (mode & QIODevice::NewOnly) ? CREATE_NEW : OPEN_ALWAYS;
  HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path::join(impl_->path, name)).utf16()),
      (reading ? GENERIC_READ : 0) | (writing ? GENERIC_WRITE : 0) | (mutation ? DELETE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      creation, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) ioError(QStringLiteral("Open pinned regular file"));
  BY_HANDLE_FILE_INFORMATION st{};
  if (!GetFileInformationByHandle(handle, &st) || GetFileType(handle) != FILE_TYPE_DISK ||
      (st.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
    CloseHandle(handle); fail(QStringLiteral("Opened object is not a regular non-link file"));
  }
  const int fd = _open_osfhandle(intptr_t(handle), _O_BINARY | (reading && writing ? _O_RDWR : writing ? _O_WRONLY : _O_RDONLY));
  if (fd < 0) { CloseHandle(handle); fail(QStringLiteral("Cannot adopt pinned file handle")); }
#else
  Q_UNUSED(mutation)
  int flags = O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (reading && writing ? O_RDWR : writing ? O_WRONLY : O_RDONLY);
  if (writing && !(mode & QIODevice::ExistingOnly)) flags |= O_CREAT;
  if (mode & QIODevice::NewOnly) flags |= O_EXCL | O_CREAT;
  if (mode & QIODevice::Append) flags |= O_APPEND;
  const int fd = ::openat(impl_->fd, QFile::encodeName(name).constData(), flags, 0600);
  if (fd < 0) ioError(QStringLiteral("Open pinned regular file"));
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); fail(QStringLiteral("Opened object is not a regular non-link file")); }
#endif
  auto file = std::shared_ptr<QFile>(new QFile, [parent = impl_](QFile *value) { delete value; });
  const auto adoptedMode = mode & ~(QIODevice::NewOnly | QIODevice::ExistingOnly | QIODevice::Truncate);
  if (!file->open(fd, adoptedMode, QFileDevice::AutoCloseHandle)) {
#ifdef Q_OS_WIN
    _close(fd);
#else
    ::close(fd);
#endif
    fail(file->errorString());
  }
  if (writing && ((mode & QIODevice::Truncate) || (!reading && !(mode & QIODevice::Append))))
    if (!file->resize(0)) fail(file->errorString());
  if ((mode & QIODevice::Append) && !file->seek(file->size())) fail(file->errorString());
  return file;
}

bool Directory::renameTo(const QString &name, const Directory &destination, const QString &target, bool replace,
                         QString *error, QFile *openedSource, const Directory *openedDirectory) const {
  leaf(name); leaf(target); ensureCurrent(); destination.ensureCurrent();
  bool ok = false;
#ifdef Q_OS_WIN
  HANDLE handle = openedSource ? HANDLE(_get_osfhandle(openedSource->handle())) : openedDirectory ? openedDirectory->impl_->handle : INVALID_HANDLE_VALUE;
  const bool owned = !openedSource && !openedDirectory;
  if (owned) handle = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path::join(impl_->path, name)).utf16()),
      DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    const std::wstring wide = target.toStdWString();
    std::vector<char> storage(sizeof(FILE_RENAME_INFO) + wide.size() * sizeof(wchar_t), 0);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = replace; rename->RootDirectory = destination.impl_->handle;
    rename->FileNameLength = DWORD(wide.size() * sizeof(wchar_t)); memcpy(rename->FileName, wide.data(), rename->FileNameLength);
    ok = SetFileInformationByHandle(handle, FileRenameInfo, rename, DWORD(storage.size()));
    const DWORD code = GetLastError();
    if (owned) CloseHandle(handle);
    SetLastError(code);
  }
  if (!ok && error) *error = QStringLiteral("Rename opened entry (Windows error %1)").arg(GetLastError());
#else
  Q_UNUSED(openedSource)
  Q_UNUSED(openedDirectory)
  const auto source = QFile::encodeName(name), to = QFile::encodeName(target);
  if (replace) ok = ::renameat(impl_->fd, source.constData(), destination.impl_->fd, to.constData()) == 0;
#ifdef Q_OS_MACOS
  else ok = ::renameatx_np(impl_->fd, source.constData(), destination.impl_->fd, to.constData(), RENAME_EXCL) == 0;
#else
  else ok = ::syscall(SYS_renameat2, impl_->fd, source.constData(), destination.impl_->fd, to.constData(), 1) == 0;
#endif
  if (!ok && error) *error = QStringLiteral("Rename pinned entry: %1 (os error %2)").arg(QString::fromLocal8Bit(strerror(errno))).arg(errno);
#endif
  return ok;
}
void Directory::claim(const QString &name, const Directory &staging, const QString &target, const FileInfo &expected,
                       QFile *openedSource, const std::function<void()> &beforeRename) const {
  if (!expected.exists) fail(QStringLiteral("Cannot claim a missing entry"));
  if (beforeRename) beforeRename();
  QString error;
  if (!renameTo(name, staging, target, false, &error, openedSource)) fail(error);
  sync(); staging.sync();
  const FileInfo claimed = staging.info(target);
  if (!claimed.exists || claimed.dev != expected.dev || claimed.ino != expected.ino ||
      claimed.isFile != expected.isFile || claimed.isDir != expected.isDir || claimed.isSymlink != expected.isSymlink ||
      (!expected.isDir && (claimed.size != expected.size || claimed.mtimeNs != expected.mtimeNs)))
    fail(QStringLiteral("Claimed entry changed; preserved in staging for recovery"));
}
FileInfo Directory::selfInfo() const {
  ensureCurrent();
#ifdef Q_OS_WIN
  BY_HANDLE_FILE_INFORMATION st{};
  if (!GetFileInformationByHandle(impl_->handle, &st)) ioError(QStringLiteral("Inspect admitted directory"));
  FileInfo out; out.exists = true; out.isDir = true; out.dev = st.dwVolumeSerialNumber;
  out.ino = (quint64(st.nFileIndexHigh) << 32) | st.nFileIndexLow;
  return out;
#else
  struct stat st{};
  if (fstat(impl_->fd, &st) != 0) ioError(QStringLiteral("Inspect admitted directory"));
  FileInfo out; out.exists = true; out.isDir = S_ISDIR(st.st_mode); out.dev = st.st_dev; out.ino = st.st_ino;
  out.size = st.st_size;
#ifdef Q_OS_MACOS
  out.mtimeNs = qint64(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  out.mtimeNs = qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  return out;
#endif
}
Directory Directory::rebound(const Directory &parent, const QString &name) const {
  leaf(name); parent.ensureCurrent();
  auto next = std::make_shared<Impl>(); next->parent = parent.impl_; next->name = name;
#ifdef Q_OS_WIN
  next->path = path::join(parent.impl_->path, name); next->removal = impl_->removal;
  if (!DuplicateHandle(GetCurrentProcess(), impl_->handle, GetCurrentProcess(), &next->handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) ioError(QStringLiteral("Retain moved directory handle"));
#else
  next->fd = fcntl(impl_->fd, F_DUPFD_CLOEXEC, 3);
  if (next->fd < 0) ioError(QStringLiteral("Retain moved directory descriptor"));
#endif
  Directory result(std::move(next));
  const auto named = parent.info(name), opened = result.selfInfo();
  if (!named.isDir || named.dev != opened.dev || named.ino != opened.ino) fail(QStringLiteral("Moved directory identity changed; preserving transaction"));
  return result;
}
QString Directory::identity() const {
#ifdef Q_OS_WIN
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(impl_->handle, &info)) ioError(QStringLiteral("Read directory identity"));
  return QStringLiteral("%1-%2-%3").arg(info.dwVolumeSerialNumber).arg(info.nFileIndexHigh).arg(info.nFileIndexLow);
#else
  struct stat info{};
  if (fstat(impl_->fd, &info) != 0) ioError(QStringLiteral("Read directory identity"));
  return QStringLiteral("%1-%2").arg(quint64(info.st_dev)).arg(quint64(info.st_ino));
#endif
}

void Directory::ensureCurrent() const {
  if (!impl_->parent) return;
  Directory parent(impl_->parent);
  parent.ensureCurrent();
#ifndef Q_OS_WIN
  struct stat named{}, opened{};
  if (fstatat(impl_->parent->fd, QFile::encodeName(impl_->name).constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
      fstat(impl_->fd, &opened) != 0 || !S_ISDIR(named.st_mode) || named.st_dev != opened.st_dev || named.st_ino != opened.st_ino)
    fail(QStringLiteral("Admitted directory changed"));
#endif
}

std::vector<DirEntry> Directory::entries() const {
  ensureCurrent();
  std::vector<DirEntry> out;
#ifdef Q_OS_WIN
  WIN32_FIND_DATAW data{};
  HANDLE search = FindFirstFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path::join(impl_->path, QStringLiteral("*"))).utf16()), &data);
  if (search == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND) return out;
    ioError(QStringLiteral("Enumerate pinned directory"));
  }
  do {
    const QString name = QString::fromWCharArray(data.cFileName);
    if (name == QLatin1String(".") || name == QLatin1String("..")) continue;
    const bool link = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
    const bool dir = !link && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    out.push_back({name, !link && !dir, dir, link});
  } while (FindNextFileW(search, &data));
  const DWORD error = GetLastError();
  FindClose(search);
  if (error != ERROR_NO_MORE_FILES) { SetLastError(error); ioError(QStringLiteral("Enumerate pinned directory")); }
#else
  const int fd = ::openat(impl_->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) ioError(QStringLiteral("Reopen pinned directory"));
  DIR *dir = fdopendir(fd);
  if (!dir) { ::close(fd); ioError(QStringLiteral("Enumerate pinned directory")); }
  int error = 0;
  for (;;) {
    errno = 0;
    dirent *entry = readdir(dir);
    if (!entry) { error = errno; break; }
    const QString name = QFile::decodeName(entry->d_name);
    if (name == QLatin1String(".") || name == QLatin1String("..")) continue;
    struct stat info{};
    if (fstatat(impl_->fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) continue;
      error = errno; break;
    }
    out.push_back({name, S_ISREG(info.st_mode), S_ISDIR(info.st_mode), S_ISLNK(info.st_mode)});
  }
  closedir(dir);
  if (error) { errno = error; ioError(QStringLiteral("Enumerate pinned directory")); }
#endif
  return out;
}

QByteArray Directory::readRegular(const QString &name) const {
  auto bytes = readOptionalRegular(name);
  if (!bytes) fail(QStringLiteral("Missing regular file: %1").arg(name));
  return std::move(*bytes);
}
std::optional<QByteArray> Directory::readOptionalRegular(const QString &name) const {
  ensureCurrent();
  leaf(name);
#ifdef Q_OS_WIN
  HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path::join(impl_->path, name)).utf16()),
      GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return std::nullopt;
    ioError(QStringLiteral("Open regular source"));
  }
  auto closeFile = qScopeGuard([file] { CloseHandle(file); });
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info) || GetFileType(file) != FILE_TYPE_DISK ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
    fail(QStringLiteral("Source is not a regular non-link file"));
  }
  QByteArray bytes, chunk(65536, Qt::Uninitialized);
  for (;;) {
    DWORD count = 0;
    if (!ReadFile(file, chunk.data(), DWORD(chunk.size()), &count, nullptr)) {
      ioError(QStringLiteral("Read regular source"));
    }
    if (!count) break;
    bytes.append(chunk.constData(), count);
  }
  return bytes;
#else
  const int fd = ::openat(impl_->fd, QFile::encodeName(name).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return std::nullopt;
    ioError(QStringLiteral("Open regular source without following links"));
  }
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { ::close(fd); fail(QStringLiteral("Source is not a regular non-link file")); }
  QFile file;
  if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); fail(file.errorString()); }
  const QByteArray bytes = file.readAll();
  if (file.error() != QFileDevice::NoError) fail(file.errorString());
  return bytes;
#endif
}

std::unique_ptr<FileLock> FileLock::acquireIn(const Directory &directory, const QString &name,
                                          std::chrono::milliseconds timeout, std::chrono::milliseconds poll) {
  leaf(name);
  directory.ensureCurrent();
  std::unique_ptr<FileLock> lock(new FileLock);
  lock->directory_ = directory.impl_;
#ifdef Q_OS_WIN
  const QString filePath = path::join(directory.impl_->path, name);
  HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(filePath).utf16()),
      GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) ioError(QStringLiteral("Open directory lease"));
  lock->handle_ = intptr_t(file);
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info) || GetFileType(file) != FILE_TYPE_DISK ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
    fail(QStringLiteral("Directory lease is not a regular file"));
#else
  const int fd = ::openat(directory.impl_->fd, QFile::encodeName(name).constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) ioError(QStringLiteral("Open directory lease"));
  lock->handle_ = fd;
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) fail(QStringLiteral("Directory lease is not a regular file"));
#endif
  const auto started = std::chrono::steady_clock::now();
  while (!lock->tryLock(false)) {
    if (std::chrono::steady_clock::now() - started >= timeout) fail(QStringLiteral("File lease timed out"));
    std::this_thread::sleep_for(poll);
  }
  return lock;
}

void Directory::writeAtomic(const QString &name, const QByteArray &bytes,
                            const std::function<void(const QString &)> &beforePublication,
                            const std::function<void(const QString &)> &beforePrivatePublication) const {
  ensureCurrent();
  leaf(name);
  const QString temporary = QStringLiteral(".room237-write-") + QUuid::createUuid().toString(QUuid::Id128);
#ifdef Q_OS_WIN
  const QString temporaryPath = path::join(impl_->path, temporary);
  HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(temporaryPath).utf16()),
      GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) ioError(QStringLiteral("Create exclusive temporary"));
  try {
    qint64 at = 0;
    while (at < bytes.size()) {
      DWORD count = 0;
      if (!WriteFile(file, bytes.constData() + at, DWORD(std::min<qint64>(bytes.size() - at, 1 << 20)), &count, nullptr) || !count)
        ioError(QStringLiteral("Write exclusive temporary"));
      at += count;
    }
    if (!FlushFileBuffers(file)) ioError(QStringLiteral("Sync exclusive temporary"));
    const std::wstring target = name.toStdWString();
    std::vector<char> storage(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = TRUE;
    rename->RootDirectory = impl_->handle;
    rename->FileNameLength = DWORD(target.size() * sizeof(wchar_t));
    memcpy(rename->FileName, target.data(), rename->FileNameLength);
    if (beforePublication) beforePublication(temporary);
    if (beforePrivatePublication) beforePrivatePublication(temporary);
    if (!SetFileInformationByHandle(file, FileRenameInfo, rename, DWORD(storage.size()))) ioError(QStringLiteral("Publish exclusive temporary"));
  } catch (...) {
    FILE_DISPOSITION_INFO remove{TRUE};
    SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
    CloseHandle(file); throw;
  }
  CloseHandle(file);
#else
  const QByteArray temp = QFile::encodeName(temporary);
  const int fd = ::openat(impl_->fd, temp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) ioError(QStringLiteral("Create exclusive temporary"));
  try {
    qint64 at = 0;
    while (at < bytes.size()) {
      const ssize_t count = ::write(fd, bytes.constData() + at, size_t(bytes.size() - at));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) ioError(QStringLiteral("Write exclusive temporary"));
      at += count;
    }
#ifdef Q_OS_MACOS
    if (::fcntl(fd, F_FULLFSYNC) != 0 && ::fsync(fd) != 0) ioError(QStringLiteral("Sync exclusive temporary"));
#else
    if (::fsync(fd) != 0) ioError(QStringLiteral("Sync exclusive temporary"));
#endif
    struct stat opened{}, named{};
    if (fstat(fd, &opened) != 0 || fstatat(impl_->fd, temp.constData(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(named.st_mode) || opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
      fail(QStringLiteral("Temporary file changed before publication"));
    QFile held;
    if (!held.open(fd, QIODevice::WriteOnly, QFileDevice::DontCloseHandle)) fail(held.errorString());
    const FileInfo expected = openedFileInfo(held);
    ensureCurrent();
    if (beforePublication) beforePublication(temporary);
    // Claim the current named entry before trusting it. A writer may have exchanged it after fstatat above. The
    // claimed entry is retained on mismatch; in particular the exception path must not unlink that replacement.
    const QString claimName = QStringLiteral(".room237-write-claim-") + QUuid::createUuid().toString(QUuid::Id128);
    Directory staging = privateChild(claimName);
    claim(temporary, staging, QStringLiteral("entry"), expected);
    if (beforePrivatePublication) beforePrivatePublication(path::join(claimName, "entry"));
    QString error;
    if (!staging.renameTo(QStringLiteral("entry"), *this, name, true, &error)) fail(error);
    const auto published = info(name);
    if (!published.isFile || published.dev != expected.dev || published.ino != expected.ino) {
      if (published.exists) claim(name, staging, "entry", published);
      fail(QStringLiteral("Atomic publication changed; preserving transaction"));
    }
    if (::fsync(impl_->fd) != 0) ioError(QStringLiteral("Sync publication directory"));
    removeEmpty(claimName, &staging);
  } catch (...) { ::close(fd); throw; }
  ::close(fd);
#endif
}

bool Directory::removeFileIfPresent(const QString &name, const FileInfo *expected, QFile *opened) const {
  ensureCurrent();
  leaf(name);
#ifdef Q_OS_WIN
  const QString filePath = path::join(impl_->path, name);
  const bool owned = !opened;
  HANDLE file = opened ? HANDLE(_get_osfhandle(opened->handle())) : CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(filePath).utf16()),
      DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return false;
    ioError(QStringLiteral("Open deletion target"));
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file, &info)) { if (owned) CloseHandle(file); ioError(QStringLiteral("Inspect deletion target")); }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    if (owned) CloseHandle(file); fail(QStringLiteral("File deletion target is a directory"));
  }
  if (expected && (expected->dev != info.dwVolumeSerialNumber || expected->ino != ((quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow))) {
    if (owned) CloseHandle(file); fail(QStringLiteral("Deletion entry changed; preserving it"));
  }
  FILE_DISPOSITION_INFO remove{TRUE};
  const bool ok = SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
  const DWORD error = GetLastError();
  if (owned) CloseHandle(file);
  else if (ok) opened->close();  // Complete deletion once this operation's DELETE-capable file handle is no longer needed.
  if (!ok) { SetLastError(error); ioError(QStringLiteral("Delete opened file")); }
  return true;
#else
  Q_UNUSED(opened)
  if (expected) {
    const auto named = info(name);
    if (!named.exists) return false;
    if (named.dev != expected->dev || named.ino != expected->ino || named.size != expected->size || named.mtimeNs != expected->mtimeNs)
      fail(QStringLiteral("Deletion entry changed; preserving it"));
  }
  if (::unlinkat(impl_->fd, QFile::encodeName(name).constData(), 0) == 0) return true;
  if (errno == ENOENT) return false;
  ioError(QStringLiteral("Delete directory entry"));
#endif
}

void Directory::removeEntry(const QString &name, const QString &expectedDirectoryId, const Directory *admitted) const {
  ensureCurrent();
  leaf(name);
#ifdef Q_OS_WIN
  // Hold the deletion handle before enumeration. Its sharing mode prevents replacement of this directory.
  const QString filePath = path::join(impl_->path, name);
  const auto wide = QDir::toNativeSeparators(filePath).toStdWString();
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) ioError(QStringLiteral("Inspect deletion entry"));
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    if (!expectedDirectoryId.isEmpty()) fail(QStringLiteral("Recursive deletion target changed"));
    removeFileIfPresent(name); return;
  }
  auto target = admitted ? admitted->impl_ : std::make_shared<Impl>();
  if (admitted && (!target->parent || Directory(target->parent).identity() != identity() || target->path != filePath || !target->removal))
    fail(QStringLiteral("Deletion capability does not belong to this parent"));
  if (!admitted) {
    target->parent = impl_; target->path = filePath; target->name = name;
    target->handle = CreateFileW(wide.c_str(), DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  }
  if (target->handle == INVALID_HANDLE_VALUE) ioError(QStringLiteral("Open recursive deletion target"));
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(target->handle, &info)) ioError(QStringLiteral("Inspect recursive deletion target"));
  if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    fail(QStringLiteral("Recursive deletion target changed"));
  Directory directory(target);
  if (!expectedDirectoryId.isEmpty() && directory.identity() != expectedDirectoryId) fail(QStringLiteral("Recursive deletion target changed"));
  for (const DirEntry &entry : directory.entries()) directory.removeEntry(entry.name);
  FILE_DISPOSITION_INFO remove{TRUE};
  if (!SetFileInformationByHandle(target->handle, FileDispositionInfo, &remove, sizeof(remove))) ioError(QStringLiteral("Delete opened directory"));
#else
  const QByteArray encoded = QFile::encodeName(name);
  struct stat before{};
  if (fstatat(impl_->fd, encoded.constData(), &before, AT_SYMLINK_NOFOLLOW) != 0) ioError(QStringLiteral("Inspect deletion entry"));
  if (!S_ISDIR(before.st_mode)) {
    if (!expectedDirectoryId.isEmpty()) fail(QStringLiteral("Recursive deletion target changed"));
    removeFileIfPresent(name); return;
  }
  Directory directory = admitted ? Directory(admitted->impl_) : child(name);
  struct stat opened{};
  if (fstat(directory.impl_->fd, &opened) != 0 || opened.st_dev != before.st_dev || opened.st_ino != before.st_ino)
    fail(QStringLiteral("Recursive deletion target changed"));
  if (!expectedDirectoryId.isEmpty() && directory.identity() != expectedDirectoryId) fail(QStringLiteral("Recursive deletion target changed"));
  for (const DirEntry &entry : directory.entries()) directory.removeEntry(entry.name);
  struct stat current{};
  if (fstatat(impl_->fd, encoded.constData(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
      current.st_dev != opened.st_dev || current.st_ino != opened.st_ino) fail(QStringLiteral("Recursive deletion target changed"));
  // unlinkat never follows the leaf. A concurrent replacement can only cause failure or remove an empty entry;
  // recursive work above always used the admitted directory descriptor.
  if (::unlinkat(impl_->fd, encoded.constData(), AT_REMOVEDIR) != 0) ioError(QStringLiteral("Remove opened directory entry"));
#endif
}
void Directory::removeEmpty(const QString &name, const Directory *admitted) const {
  leaf(name); ensureCurrent();
  Directory target = admitted ? admitted->share() : childForRemoval(name);
  if (!target.entries().empty()) fail(QStringLiteral("Directory is not empty; preserving it"));
#ifdef Q_OS_WIN
  if (!target.impl_->removal) fail(QStringLiteral("Directory was not admitted for removal"));
  FILE_DISPOSITION_INFO remove{TRUE};
  if (!SetFileInformationByHandle(target.impl_->handle, FileDispositionInfo, &remove, sizeof(remove))) ioError(QStringLiteral("Remove empty opened directory"));
#else
  const auto current = info(name);
  if (!current.isDir || QStringLiteral("%1-%2").arg(current.dev).arg(current.ino) != target.identity()) fail(QStringLiteral("Empty directory identity changed"));
  if (unlinkat(impl_->fd, QFile::encodeName(name).constData(), AT_REMOVEDIR) != 0) ioError(QStringLiteral("Remove empty pinned directory"));
#endif
}
}  // namespace core
