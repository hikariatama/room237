#pragma once

#include "fs.h"
#include <map>
#include <mutex>

namespace core {

// A per-operation filesystem view. Logical names remain unchanged (including the vault/SQLite associated data),
// while every lookup below an admitted root is relative to a retained directory capability.
class FileAccess {
public:
  struct State;
  explicit FileAccess(const QStringList &roots, bool resolveRootAliases = false);
  void bindDirectory(const QString &path, const std::shared_ptr<Directory> &directory);
  explicit FileAccess(std::shared_ptr<State> state);
  ~FileAccess();
  FileAccess(const FileAccess &) = delete;
  static FileAccess *current(const QString &path);
  static std::shared_ptr<State> capture();
  static std::shared_ptr<State> snapshot(const std::shared_ptr<State> &state);
  std::optional<QString> locateDirectory(const QString &originalPath, const QString &identity);
  void bindPublished(const QString &path, const std::shared_ptr<QFile> &file, const std::shared_ptr<Directory> &parent);
  void releasePublished(const QString &path);  // Keep the admitted identity; reopen and verify before later use.
  void unbindDirectory(const QString &path);
  std::shared_ptr<Directory> boundDirectory(const QString &path) const;
  static bool rootCurrent(const std::shared_ptr<State> &state, const QString &path);
  QString canonicalSpelling(const QString &path) const;
  std::shared_ptr<Directory> directory(const QString &path);
  std::shared_ptr<QFile> open(const QString &path, QIODevice::OpenMode mode);
  std::shared_ptr<QFile> pin(const QString &path, bool mutation = false);
  void unpin(const QString &path);
  void bind(const QString &path, const std::shared_ptr<QFile> &file);
  std::shared_ptr<QFile> pinned(const QString &path) const;
  // Called only after synchronous decoders/readers of this operation have returned. Windows needs to release its
  // read-only sharing guard before taking a DELETE-capable claim handle; the claim still checks the admitted id.
  void releaseReaders(const FileInfo &object);
  FileInfo info(const QString &path, bool followLeaf = false);
  void observeDirectory(const QString &path);
  QString decoderPath(const QString &path, bool output = false);
  void setHook(std::function<void(const QString &, const QString &, const QString &)> hook);
  void checkpoint(const QString &phase, const QString &source, const QString &target = {});
  std::vector<int> decoderDescriptors() const;
  static std::vector<int> inheritedDecoderDescriptors();
private:
  bool contains(const QString &path) const;
  std::shared_ptr<State> state_;
  FileAccess *previous_ = nullptr;
  std::map<QString, std::weak_ptr<Directory>> directories_;
  std::map<QString, std::shared_ptr<Directory>> boundDirectories_;
  std::map<QString, std::shared_ptr<QFile>> files_;
  std::map<QString, FileInfo> published_;
  std::vector<std::shared_ptr<QFile>> decoderFiles_;
  std::vector<std::weak_ptr<QFile>> openedFiles_;
};

// Opens into an existing QFile without resolving an admitted pathname again. Outside a FileAccess operation this
// has QFile's usual behavior. All callers retain their logical names for format detection and associated data.
bool openFile(QFile &file, const QString &path, QIODevice::OpenMode mode);
QString decoderPath(const QString &path, bool output = false);
FileInfo openedFileInfo(const QFile &file);
void syncOpenedFile(QFile &file);
// Orders the file's data before later writes (such as its publication by rename) without flushing the drive cache;
// a later full sync makes everything before it durable. macOS: F_BARRIERFSYNC; elsewhere the same as syncOpenedFile.
void syncOpenedFileOrdered(QFile &file);

}  // namespace core
