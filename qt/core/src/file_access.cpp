#include "file_access.h"

#include <QDir>
#include <QScopeGuard>
#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <sys/syscall.h>
#endif
#endif

namespace core {
namespace {
thread_local FileAccess *active = nullptr;
int duplicate(int fd) {
#ifdef Q_OS_WIN
  return _dup(fd);
#else
  return fcntl(fd, F_DUPFD_CLOEXEC, 3);
#endif
}
void closeDescriptor(int fd) {
#ifdef Q_OS_WIN
  _close(fd);
#else
  ::close(fd);
#endif
}
bool adopt(QFile &file, int fd, QIODevice::OpenMode mode) {
  if (fd < 0) fail(QStringLiteral("Cannot duplicate admitted file"));
  if (!file.open(fd, mode & ~(QIODevice::NewOnly | QIODevice::ExistingOnly | QIODevice::Truncate), QFileDevice::AutoCloseHandle)) {
    closeDescriptor(fd); return false;
  }
  if ((mode & QIODevice::Truncate) && !file.resize(0)) return false;
  if (!(mode & QIODevice::Append)) file.seek(0);
  return true;
}
}  // namespace

struct FileAccess::State {
  std::map<QString, std::shared_ptr<Directory>> roots;
  std::map<QString, QString> canonicalRoots;
  std::map<QString, QString> directoryIds;
  std::mutex mutex;
  std::function<void(const QString &, const QString &, const QString &)> hook;
};

FileAccess::FileAccess(const QStringList &roots, bool resolveRootAliases) : previous_(active) {
  state_ = std::make_shared<State>();
  for (const QString &root : roots) {
    auto *inherited = current(root);
    const QString real = inherited ? inherited->canonicalSpelling(root) : resolveRootAliases ? canonical(root) : root;
    auto cap = inherited ? inherited->directory(root) : std::make_shared<Directory>(real);
    cap->nativeHandle();
    state_->roots.emplace(root, cap); state_->roots.emplace(real, cap);
    state_->canonicalRoots.emplace(root, real); state_->canonicalRoots.emplace(real, real);
  }
  for (auto *view = previous_; view; view = view->previous_)
    for (const auto &[name, cap] : view->boundDirectories_)
      if (contains(name)) { boundDirectories_.try_emplace(name, cap); directories_.try_emplace(name, cap); }
  active = this;
}
void FileAccess::bindDirectory(const QString &path, const std::shared_ptr<Directory> &directory) {
  const QString key = canonicalSpelling(path);
  directories_[key] = directory; boundDirectories_[key] = directory;
}
void FileAccess::unbindDirectory(const QString &path) { const auto key = canonicalSpelling(path); boundDirectories_.erase(key); directories_.erase(key); }
std::shared_ptr<Directory> FileAccess::boundDirectory(const QString &path) const {
  const auto key = canonicalSpelling(path);
  for (auto *view = this; view; view = view->previous_)
    if (auto found = view->boundDirectories_.find(key); found != view->boundDirectories_.end()) return found->second;
  return {};
}
FileAccess::FileAccess(std::shared_ptr<State> state) : state_(std::move(state)), previous_(active) { active = this; }
FileAccess::~FileAccess() { active = previous_; }
bool FileAccess::contains(const QString &path) const {
  if (state_)
    for (const auto &[root, directory] : state_->roots)
      if (path::startsWith(path, root)) return true;
  return false;
}
FileAccess *FileAccess::current(const QString &path) {
  for (auto *view = active; view; view = view->previous_)
    if (view->contains(path)) return view;
  return nullptr;
}
std::shared_ptr<FileAccess::State> FileAccess::capture() {
  for (auto *view = active; view; view = view->previous_) if (view->state_) return view->state_;
  return nullptr;
}
std::shared_ptr<FileAccess::State> FileAccess::snapshot(const std::shared_ptr<State> &state) {
  if (!state) return nullptr;
  auto copy = std::make_shared<State>();
  std::lock_guard lock(state->mutex);
  copy->roots = state->roots; copy->canonicalRoots = state->canonicalRoots; copy->hook = state->hook;
  return copy;
}
std::optional<QString> FileAccess::locateDirectory(const QString &originalPath, const QString &identity) {
  const QString original = canonicalSpelling(originalPath);
  std::shared_ptr<Directory> root; QString rootPath;
  for (const auto &[name, cap] : state_->roots) {
    const QString canonical = canonicalSpelling(name);
    if (path::startsWith(original, canonical) && canonical.size() > rootPath.size()) { root = cap; rootPath = canonical; }
  }
  if (!root) fail(QStringLiteral("Album is outside the admitted root"));
  auto inspect = [&](const QString &candidate) -> std::shared_ptr<Directory> {
    try {
      auto cap = std::make_shared<Directory>(root->descend(path::relative(candidate, rootPath)));
      if (cap->identity() == identity) return cap;
    } catch (const Error &) {}  // A renamed/missing child is searched by its admitted identity below, never followed.
    return {};
  };
  QString known;
  { std::lock_guard lock(state_->mutex); for (const auto &[name, id] : state_->directoryIds) if (id == identity) { known = name; if (name == original) break; } }
  auto admit = [&](const QString &candidate, const std::shared_ptr<Directory> &cap) {
    {
      std::lock_guard lock(state_->mutex);
      if (candidate != original) {
        std::vector<std::pair<QString, QString>> rebased;
        for (auto it = state_->directoryIds.begin(); it != state_->directoryIds.end();) {
          if (path::startsWith(it->first, original)) {
            const QString suffix = path::relative(it->first, original);
            rebased.emplace_back(suffix.isEmpty() ? candidate : path::join(candidate, suffix), it->second);
            it = state_->directoryIds.erase(it);
          } else ++it;
        }
        for (const auto &[name, id] : rebased) state_->directoryIds[name] = id;
      }
      state_->directoryIds[candidate] = identity;
    }
    bindDirectory(candidate, cap); return std::optional<QString>(candidate);
  };
  if (auto cap = inspect(known.isEmpty() ? original : known)) return admit(known.isEmpty() ? original : known, cap);
  QStringList pending{QString()};
  while (!pending.empty()) {
    const QString relative = pending.takeLast();
    std::optional<Directory> parent;
    try { parent.emplace(root->descend(relative)); } catch (const Error &) { continue; }
    for (const auto &entry : parent->entries()) {
      if (!entry.isDir || entry.isSymlink || entry.name.startsWith(".room237-")) continue;
      const QString childRelative = path::join(relative, entry.name), candidate = path::join(rootPath, childRelative);
      try {
        auto child = std::make_shared<Directory>(parent->child(entry.name));
        if (child->identity() == identity) return admit(candidate, child);
        pending << childRelative;
      } catch (const Error &) {}  // Concurrent substitutions cannot escape the retained root descriptor.
    }
  }
  return std::nullopt;  // A deleted admitted album has no queued work to perform.
}
bool FileAccess::rootCurrent(const std::shared_ptr<State> &state, const QString &path) {
  if (!state) return true;
  try {
    for (const auto &[root, cap] : state->roots) if (path::startsWith(path, root)) { cap->nativeHandle(); return true; }
  } catch (const Error &) { return false; }
  return false;
}
QString FileAccess::canonicalSpelling(const QString &path) const {
  QString prefix, canonical;
  for (const auto &[root, real] : state_->canonicalRoots)
    if (path::startsWith(path, root) && root.size() > prefix.size()) { prefix = root; canonical = real; }
  const QString relative = path::relative(path, prefix);
  return relative.isEmpty() ? canonical : path::join(canonical, relative);
}
std::shared_ptr<Directory> FileAccess::directory(const QString &path) {
  const QString key = canonicalSpelling(path);
  if (key != path) return directory(key);
  if (const auto cached = directories_.find(path); cached != directories_.end()) {
    // entries() and every effect revalidate the retained chain; identity alone intentionally describes the handle.
    if (auto retained = cached->second.lock()) return retained;
  }
  std::shared_ptr<Directory> root;
  QString rootPath;
  for (const auto &[name, candidate] : state_->roots)
    if (path::startsWith(path, name) && name.size() > rootPath.size()) { root = candidate; rootPath = name; }
  for (auto *view = this; view && view->state_ == state_; view = view->previous_)
    for (const auto &[name, candidate] : view->boundDirectories_)
      if (path::startsWith(path, name) && name.size() > rootPath.size()) { root = candidate; rootPath = name; }
  if (!root) fail(QStringLiteral("Path is outside the admitted filesystem view"));
  auto opened = std::make_shared<Directory>(root->descend(path::relative(path, rootPath)));
  {
    std::lock_guard lock(state_->mutex);
    const auto expected = state_->directoryIds.find(path);
    if (expected != state_->directoryIds.end() && expected->second != opened->identity())
      fail(QStringLiteral("Admitted album identity changed"));
  }
  directories_[path] = opened;
  return opened;
}
void FileAccess::observeDirectory(const QString &path) {
  auto opened = directory(path);
  std::lock_guard lock(state_->mutex);
  const auto key = canonicalSpelling(path);
  auto [it, added] = state_->directoryIds.emplace(key, opened->identity());
  if (!added && it->second != opened->identity()) fail(QStringLiteral("Album identity changed during discovery"));
  // Discovery retains identities, rather than one fd per album. Workers pin only their active album and ancestors.
  directories_.erase(key);
}
std::shared_ptr<QFile> FileAccess::open(const QString &path, QIODevice::OpenMode mode) {
  const QString key = canonicalSpelling(path);
  if (!files_.contains(key) && !published_.contains(key) && previous_ && previous_->state_ == state_) return previous_->open(path, mode);
  if (auto found = files_.find(key); found != files_.end()) {
    if ((mode & QIODevice::WriteOnly) && !(found->second->openMode() & QIODevice::WriteOnly)) {
      auto writing = directory(path::parent(path))->openRegular(path::fileName(path), (mode | QIODevice::ReadOnly | QIODevice::ExistingOnly) & ~QIODevice::Truncate);
      const auto expected = openedFileInfo(*found->second), actual = openedFileInfo(*writing);
      if (expected.dev != actual.dev || expected.ino != actual.ino) fail(QStringLiteral("Admitted file changed before writable open"));
      if ((mode & QIODevice::Truncate) || !(mode & (QIODevice::ReadOnly | QIODevice::Append)))
        if (!writing->resize(0)) fail(writing->errorString());
      openedFiles_.push_back(writing);
      return writing;
    }
    auto copy = std::make_shared<QFile>();
    if (!adopt(*copy, duplicate(found->second->handle()), mode)) fail(copy->errorString());
    openedFiles_.push_back(copy);
    return copy;
  }
  const auto expected = published_.find(key);
  const auto opening = expected != published_.end() && (mode & QIODevice::WriteOnly) ? (mode | QIODevice::ReadOnly | QIODevice::ExistingOnly) & ~QIODevice::Truncate : mode;
  auto file = directory(path::parent(path))->openRegular(path::fileName(path), opening);
  if (expected != published_.end()) {
    const auto actual = openedFileInfo(*file);
    if (actual.dev != expected->second.dev || actual.ino != expected->second.ino) fail(QStringLiteral("Published file changed before reopen; preserving journal"));
    if ((mode & QIODevice::Truncate) || ((mode & QIODevice::WriteOnly) && !(mode & (QIODevice::ReadOnly | QIODevice::Append))))
      if (!file->resize(0)) fail(file->errorString());
  }
  openedFiles_.push_back(file);
  return file;
}
std::shared_ptr<QFile> FileAccess::pin(const QString &path, bool mutation) {
  const QString key = canonicalSpelling(path);
  for (auto *view = this; view && view->state_ == state_; view = view->previous_)
    if (auto file = view->files_.find(key); file != view->files_.end()) { files_[key] = file->second; return file->second; }
  for (auto *view = this; view && view->state_ == state_; view = view->previous_)
    if (const auto expected = view->published_.find(key); expected != view->published_.end()) { published_[key] = expected->second; break; }
  auto file = directory(path::parent(path))->openRegular(path::fileName(path), QIODevice::ReadOnly, mutation);
  if (const auto expected = published_.find(key); expected != published_.end()) {
    const auto actual = openedFileInfo(*file);
    if (actual.dev != expected->second.dev || actual.ino != expected->second.ino) fail(QStringLiteral("Published file changed before pin; preserving journal"));
  }
  openedFiles_.push_back(file);
  files_[key] = file;
  return file;
}
void FileAccess::unpin(const QString &path) { const auto key = canonicalSpelling(path); files_.erase(key); published_.erase(key); }
void FileAccess::bind(const QString &path, const std::shared_ptr<QFile> &file) { files_[canonicalSpelling(path)] = file; openedFiles_.push_back(file); }
void FileAccess::bindPublished(const QString &path, const std::shared_ptr<QFile> &file, const std::shared_ptr<Directory> &parent) {
#ifdef Q_OS_WIN
  // A DELETE-capable claim handle cannot coexist with a new no-delete-sharing reader. Retire the synchronous
  // operation's handles, then admit the published name without truncation and verify it before any further use.
  // A replacement in that gap is preserved and refused; it is never used by a decoder or metadata writer.
  const auto expected = openedFileInfo(*file);
  const auto mode = file->openMode() & (QIODevice::ReadOnly | QIODevice::WriteOnly);
  releaseReaders(expected); file->close();
  auto anchor = parent->openRegular(core::path::fileName(path), mode | QIODevice::ExistingOnly);
  const auto actual = openedFileInfo(*anchor);
  if (actual.dev != expected.dev || actual.ino != expected.ino) fail(QStringLiteral("Published file changed during reader admission; preserving journal"));
#else
  auto anchor = std::shared_ptr<QFile>(new QFile, [parent](QFile *value) { delete value; });
  if (!adopt(*anchor, duplicate(file->handle()), file->openMode())) fail(anchor->errorString());
#endif
  bind(path, anchor);
}
void FileAccess::releasePublished(const QString &path) {
  const QString key = canonicalSpelling(path);
  if (auto file = files_.find(key); file != files_.end()) {
    published_[key] = openedFileInfo(*file->second);
    files_.erase(file);
  }
}
std::shared_ptr<QFile> FileAccess::pinned(const QString &path) const {
  const QString key = canonicalSpelling(path);
  for (auto *view = this; view && view->state_ == state_; view = view->previous_)
    if (auto found = view->files_.find(key); found != view->files_.end()) return found->second;
  return {};
}
void FileAccess::releaseReaders(const FileInfo &object) {
  for (auto *view = this; view && view->state_ == state_; view = view->previous_)
    for (const auto &weak : view->openedFiles_) if (auto file = weak.lock(); file && file->isOpen()) {
      const auto info = openedFileInfo(*file);
      if (info.dev == object.dev && info.ino == object.ino) file->close();
    }
}
FileInfo FileAccess::info(const QString &path, bool followLeaf) {
  for (const auto &[root, cap] : state_->roots)
    if (path == root) { FileInfo result; result.exists = true; result.isDir = true; return result; }
  if (!info(path::parent(path)).isDir) return {};
  const auto named = directory(path::parent(path))->info(path::fileName(path), followLeaf);
  const auto key = canonicalSpelling(path);
  for (auto *view = this; view && view->state_ == state_; view = view->previous_)
    if (const auto expected = view->published_.find(key); expected != view->published_.end()) {
      if (!named.exists || named.dev != expected->second.dev || named.ino != expected->second.ino) fail(QStringLiteral("Published file name changed"));
      break;
    }
  if (auto file = pinned(path)) {
    if (!file->isOpen()) fail(QStringLiteral("Admitted file reader has already closed"));
    const auto opened = openedFileInfo(*file);
    if (!named.exists || named.dev != opened.dev || named.ino != opened.ino) fail(QStringLiteral("Admitted file name changed"));
  }
  return named;
}
QString FileAccess::decoderPath(const QString &path, bool output) {
  auto file = open(path, output ? QIODevice::ReadWrite | QIODevice::ExistingOnly : QIODevice::ReadOnly);
  decoderFiles_.push_back(file);
#ifdef Q_OS_WIN
  // The file handle denies delete sharing, and the retained directory chain denies replacement/reparse changes.
  return path;
#else
  return QStringLiteral("/dev/fd/%1").arg(file->handle());
#endif
}
std::vector<int> FileAccess::decoderDescriptors() const {
  std::vector<int> descriptors;
  for (const auto &file : decoderFiles_) descriptors.push_back(file->handle());
  return descriptors;
}
std::vector<int> FileAccess::inheritedDecoderDescriptors() {
  std::vector<int> out;
  for (auto *view = active; view; view = view->previous_) {
    auto descriptors = view->decoderDescriptors();
    out.insert(out.end(), descriptors.begin(), descriptors.end());
  }
  return out;
}
bool openFile(QFile &file, const QString &path, QIODevice::OpenMode mode) {
  if (auto *access = FileAccess::current(path)) {
    if (!(mode & QIODevice::WriteOnly) && !access->pinned(path) && !access->info(path).exists) return false;
    auto opened = access->open(path, mode);
    return adopt(file, duplicate(opened->handle()), mode);
  }
  file.setFileName(path);
  return file.open(mode);
}
QString decoderPath(const QString &path, bool output) {
  if (auto *access = FileAccess::current(path)) return access->decoderPath(path, output);
  return path;
}
void FileAccess::setHook(std::function<void(const QString &, const QString &, const QString &)> hook) {
  std::lock_guard lock(state_->mutex); state_->hook = std::move(hook);
}
void FileAccess::checkpoint(const QString &phase, const QString &source, const QString &target) {
  std::function<void(const QString &, const QString &, const QString &)> hook;
  { std::lock_guard lock(state_->mutex); hook = state_->hook; }
  if (hook) hook(phase, source, target);
}
FileInfo openedFileInfo(const QFile &file) {
  FileInfo out;
#ifdef Q_OS_WIN
  BY_HANDLE_FILE_INFORMATION st{};
  const HANDLE handle = HANDLE(_get_osfhandle(file.handle()));
  if (!GetFileInformationByHandle(handle, &st)) fail(QStringLiteral("Cannot inspect admitted file"));
  auto ns = [](const FILETIME &t) -> qint64 { return (((qint64(t.dwHighDateTime) << 32) | t.dwLowDateTime) - 116444736000000000LL) * 100; };
  out.exists = true; out.isDir = st.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
  out.isSymlink = st.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT; out.isFile = !out.isDir && !out.isSymlink;
  out.dev = st.dwVolumeSerialNumber; out.ino = (quint64(st.nFileIndexHigh) << 32) | st.nFileIndexLow;
  out.size = (quint64(st.nFileSizeHigh) << 32) | st.nFileSizeLow;
  out.mtimeNs = ns(st.ftLastWriteTime); out.birthSec = ns(st.ftCreationTime) / 1000000000;
  out.ctimeSec = out.mtimeNs / 1000000000;
#else
  struct stat st{};
  if (fstat(file.handle(), &st) != 0) fail(QStringLiteral("Cannot inspect admitted file"));
  out.exists = true; out.isFile = S_ISREG(st.st_mode); out.isDir = S_ISDIR(st.st_mode); out.isSymlink = S_ISLNK(st.st_mode);
  out.dev = st.st_dev; out.ino = st.st_ino; out.size = st.st_size; out.readOnly = !(st.st_mode & S_IWUSR);
#ifdef Q_OS_MACOS
  out.mtimeNs = qint64(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
  out.ctimeSec = st.st_ctimespec.tv_sec; out.ctimeNsec = st.st_ctimespec.tv_nsec; out.birthSec = st.st_birthtimespec.tv_sec;
#else
  out.mtimeNs = qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
  out.ctimeSec = st.st_ctim.tv_sec; out.ctimeNsec = st.st_ctim.tv_nsec;
#if defined(Q_OS_LINUX) && defined(STATX_BTIME) && defined(SYS_statx)
  struct statx extended{};
  if (syscall(SYS_statx, file.handle(), "", AT_EMPTY_PATH, STATX_BTIME, &extended) == 0 && (extended.stx_mask & STATX_BTIME)) out.birthSec = extended.stx_btime.tv_sec;
#endif
#endif
#endif
  return out;
}
void syncOpenedFileOrdered(QFile &file) {
#ifdef Q_OS_MACOS
  if (!file.flush()) fail(file.errorString());
  if (fcntl(file.handle(), F_BARRIERFSYNC) != 0 && fsync(file.handle()) != 0) fail(QStringLiteral("Cannot sync admitted file"));
#else
  syncOpenedFile(file);
#endif
}
void syncOpenedFile(QFile &file) {
  if (!file.flush()) fail(file.errorString());
#ifdef Q_OS_WIN
  if (!FlushFileBuffers(HANDLE(_get_osfhandle(file.handle())))) fail(QStringLiteral("Cannot sync admitted file"));
#elif defined(Q_OS_MACOS)
  if (fcntl(file.handle(), F_FULLFSYNC) != 0 && fsync(file.handle()) != 0) fail(QStringLiteral("Cannot sync admitted file"));
#else
  if (fsync(file.handle()) != 0) fail(QStringLiteral("Cannot sync admitted file"));
#endif
}
}  // namespace core
