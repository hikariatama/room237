#pragma once
// Extended attributes of a directory entry (never through a symlink), and of an open file. On Windows the
// named value lives in an alternate data stream.
#include <QByteArray>
#include <QString>
#include <optional>

namespace core::xattr {

struct Result {
  std::optional<QByteArray> value;
  int error = 0;  // errno; 0 on success or when the attribute is absent.
  bool permissionDenied() const;
};
Result get(const QString &path, const char *name);
void set(const QString &path, const char *name, const QByteArray &value);
void remove(const QString &path, const char *name);
QStringList list(const QString &path);  // Names, for preserving attributes across encryption.

// An opened object, addressed even after its path was replaced. Opens symlinks themselves where the platform can.
class OpenFile {
public:
  explicit OpenFile(const QString &path);
  ~OpenFile();
  OpenFile(const OpenFile &) = delete;
  OpenFile &operator=(const OpenFile &) = delete;
  Result get(const char *name) const;
  void set(const char *name, const QByteArray &value) const;
  void remove(const char *name) const;
  bool sameAs(const QString &path) const;  // Same device, inode, size, mtime and symlink-ness as the entry.
  struct Namespace {
    qint64 mtimeNs = 0;
    quint64 size = 0, dev = 0, ino = 0;
    bool symlink = false;
    bool operator==(const Namespace &) const = default;
  };
  Namespace identity() const;
  static std::optional<Namespace> pathIdentity(const QString &path);
private:
  int fd_ = -1;
};

}  // namespace core::xattr
