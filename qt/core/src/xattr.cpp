#include "xattr.h"

#include "fs.h"

#include <QFile>

#ifndef Q_OS_WIN
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace core::xattr {

bool Result::permissionDenied() const {
#ifndef Q_OS_WIN
  return error == EACCES || error == EPERM;
#else
  return error == 5;
#endif
}

#ifndef Q_OS_WIN
namespace {
QByteArray native(const QString &path) { return QFile::encodeName(path); }
QString errnoText(int error) { return QString::fromLocal8Bit(strerror(error)) + QStringLiteral(" (os error %1)").arg(error); }
#ifdef Q_OS_MACOS
constexpr int kNoAttr = ENOATTR;
#else
constexpr int kNoAttr = ENODATA;
#endif
template <typename Get> Result readLoop(Get get) {
  Result result;
  for (int attempt = 0; attempt < 8; ++attempt) {
    const ssize_t size = get(nullptr, 0);
    if (size < 0) {
      if (errno == kNoAttr) return result;
      result.error = errno;
      return result;
    }
    QByteArray buffer(qsizetype(size), Qt::Uninitialized);
    const ssize_t got = get(buffer.data(), size_t(buffer.size()));
    if (got >= 0) {
      buffer.truncate(qsizetype(got));
      result.value = buffer;
      return result;
    }
    if (errno == ERANGE) continue;
    if (errno == kNoAttr) return result;
    result.error = errno;
    return result;
  }
  result.error = ERANGE;
  return result;
}
}  // namespace

Result get(const QString &path, const char *name) {
  const QByteArray p = native(path);
#ifdef Q_OS_MACOS
  return readLoop([&](char *buffer, size_t size) { return getxattr(p.constData(), name, buffer, size, 0, XATTR_NOFOLLOW); });
#else
  return readLoop([&](char *buffer, size_t size) { return lgetxattr(p.constData(), name, buffer, size); });
#endif
}
void set(const QString &path, const char *name, const QByteArray &value) {
  const QByteArray p = native(path);
#ifdef Q_OS_MACOS
  const int rc = setxattr(p.constData(), name, value.constData(), size_t(value.size()), 0, XATTR_NOFOLLOW);
#else
  const int rc = lsetxattr(p.constData(), name, value.constData(), size_t(value.size()), 0);
#endif
  if (rc != 0) fail(errnoText(errno));
}
void remove(const QString &path, const char *name) {
  const QByteArray p = native(path);
#ifdef Q_OS_MACOS
  const int rc = removexattr(p.constData(), name, XATTR_NOFOLLOW);
#else
  const int rc = lremovexattr(p.constData(), name);
#endif
  if (rc != 0 && errno != kNoAttr) fail(errnoText(errno));
}
QStringList list(const QString &path) {
  const QByteArray p = native(path);
#ifdef Q_OS_MACOS
  ssize_t size = listxattr(p.constData(), nullptr, 0, XATTR_NOFOLLOW);
#else
  ssize_t size = llistxattr(p.constData(), nullptr, 0);
#endif
  if (size <= 0) return {};
  QByteArray buffer(qsizetype(size), Qt::Uninitialized);
#ifdef Q_OS_MACOS
  size = listxattr(p.constData(), buffer.data(), size_t(buffer.size()), XATTR_NOFOLLOW);
#else
  size = llistxattr(p.constData(), buffer.data(), size_t(buffer.size()));
#endif
  QStringList names;
  if (size <= 0) return names;
  for (const QByteArray &name : buffer.left(qsizetype(size)).split('\0'))
    if (!name.isEmpty()) names << QString::fromUtf8(name);
  return names;
}

OpenFile::OpenFile(const QString &path) {
#ifdef Q_OS_MACOS
  const int flags = O_SYMLINK;
#else
  const int flags = O_NOFOLLOW;
#endif
  const QByteArray p = native(path);
  fd_ = ::open(p.constData(), O_RDONLY | flags | O_CLOEXEC);
  if (fd_ < 0 && (errno == EACCES || errno == EPERM)) fd_ = ::open(p.constData(), O_WRONLY | flags | O_CLOEXEC);
  if (fd_ < 0) fail(errnoText(errno));
}
OpenFile::~OpenFile() {
  if (fd_ >= 0) ::close(fd_);
}
Result OpenFile::get(const char *name) const {
#ifdef Q_OS_MACOS
  return readLoop([&](char *buffer, size_t size) { return fgetxattr(fd_, name, buffer, size, 0, 0); });
#else
  return readLoop([&](char *buffer, size_t size) { return fgetxattr(fd_, name, buffer, size); });
#endif
}
void OpenFile::set(const char *name, const QByteArray &value) const {
#ifdef Q_OS_MACOS
  const int rc = fsetxattr(fd_, name, value.constData(), size_t(value.size()), 0, 0);
#else
  const int rc = fsetxattr(fd_, name, value.constData(), size_t(value.size()), 0);
#endif
  if (rc != 0) fail(errnoText(errno));
}
void OpenFile::remove(const char *name) const {
#ifdef Q_OS_MACOS
  const int rc = fremovexattr(fd_, name, 0);
#else
  const int rc = fremovexattr(fd_, name);
#endif
  if (rc != 0) fail(errnoText(errno));
}
static OpenFile::Namespace fromStat(const struct stat &st) {
  OpenFile::Namespace ns;
#ifdef Q_OS_MACOS
  ns.mtimeNs = qint64(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  ns.mtimeNs = qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  ns.size = quint64(st.st_size);
  ns.dev = quint64(st.st_dev);
  ns.ino = quint64(st.st_ino);
  ns.symlink = S_ISLNK(st.st_mode);
  return ns;
}
OpenFile::Namespace OpenFile::identity() const {
  struct stat st {};
  if (fstat(fd_, &st) != 0) fail(errnoText(errno));
  return fromStat(st);
}
std::optional<OpenFile::Namespace> OpenFile::pathIdentity(const QString &path) {
  struct stat st {};
  if (::lstat(native(path).constData(), &st) != 0) return std::nullopt;
  return fromStat(st);
}
bool OpenFile::sameAs(const QString &path) const { return pathIdentity(path) == identity(); }

#else

static QString stream(const QString &path, const char *name) {
  QString stem = QString::fromLatin1(name);
  if (stem.startsWith(QLatin1String("user.room237."))) stem = QStringLiteral("room237_") + stem.mid(13);
  if (stem == QLatin1String("room237_shoot_ts")) stem = QStringLiteral("room237_ts");
  return path + QLatin1Char(':') + stem;
}
Result get(const QString &path, const char *name) {
  QFile file(stream(path, name));
  Result result;
  if (!file.open(QIODevice::ReadOnly)) return result;
  result.value = file.readAll();
  return result;
}
void set(const QString &path, const char *name, const QByteArray &value) { writeAll(stream(path, name), value); }
void remove(const QString &path, const char *name) { QFile::remove(stream(path, name)); }
QStringList list(const QString &) { return {}; }
OpenFile::OpenFile(const QString &) { fail(QStringLiteral("Unsupported on this platform")); }
OpenFile::~OpenFile() = default;
Result OpenFile::get(const char *) const { return {}; }
void OpenFile::set(const char *, const QByteArray &) const {}
void OpenFile::remove(const char *) const {}
OpenFile::Namespace OpenFile::identity() const { return {}; }
std::optional<OpenFile::Namespace> OpenFile::pathIdentity(const QString &) { return std::nullopt; }
bool OpenFile::sameAs(const QString &) const { return false; }
#endif

}  // namespace core::xattr
