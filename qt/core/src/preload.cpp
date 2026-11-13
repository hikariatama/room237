#include "preload.h"

#include "album_db.h"
#include "constants.h"
#include "duplicates.h"
#include "host.h"
#include "media_delete.h"
#include "metadata.h"
#include "settings.h"
#include "thumb.h"
#include "util.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

namespace core::preload {

namespace {
constexpr size_t kMaxPendingMedia = 512;
constexpr size_t kCatalogPage = 256;

// Pending tasks indexed by path and scheduling order. Promotion does not scan the backlog or leave stale queue
// entries behind. High priority keeps front-insertion order; background tasks keep FIFO order.
template <typename T> class PendingTasks {
public:
  bool enqueue(const QString &path, T data, TaskPriority priority) {
    auto it = tasks_.find(path);
    if (it != tasks_.end()) {
      if (!(!it->second.first.high && priority == TaskPriority::High)) return false;
      const Key key = nextKey(priority);
      order_.erase(tasks_[path].first);
      tasks_[path].first = key;
      order_.emplace(key, path);
    } else {
      const Key key = nextKey(priority);
      order_.emplace(key, path);
      tasks_.emplace(path, std::make_pair(key, std::move(data)));
    }
    return true;
  }
  std::optional<std::pair<QString, T>> popFront() {
    if (order_.empty()) return std::nullopt;
    const auto first = order_.begin();
    const QString path = first->second;
    order_.erase(first);
    auto it = tasks_.find(path);
    std::pair<QString, T> out{path, std::move(it->second.second)};
    tasks_.erase(it);
    return out;
  }
  bool contains(const QString &path) const { return tasks_.count(path) > 0; }
  size_t size() const { return tasks_.size(); }
  bool empty() const { return tasks_.empty(); }
  size_t trimPrefix(const QString &prefix) {
    const size_t before = size();
    for (auto it = tasks_.begin(); it != tasks_.end();) {
      if (path::startsWith(it->first, prefix)) {
        order_.erase(it->second.first);
        it = tasks_.erase(it);
      } else {
        ++it;
      }
    }
    return before - size();
  }
  void clear() {
    tasks_.clear();
    order_.clear();
    sequence_ = 0;
  }

private:
  struct Key {
    bool high = false;
    quint64 sequence = 0;
    bool operator<(const Key &other) const {
      if (high != other.high) return high;           // High precedes Low.
      if (high) return sequence > other.sequence;    // Newest high first.
      return sequence < other.sequence;              // Oldest low first.
    }
  };
  Key nextKey(TaskPriority priority) {
    if (sequence_ == std::numeric_limits<quint64>::max()) {
      const quint64 count = tasks_.size();
      std::map<Key, QString> previous;
      previous.swap(order_);
      quint64 index = 0;
      for (const auto &[key, path] : previous) {
        const Key renumbered = key.high ? Key{true, count - index} : Key{false, index};
        tasks_[path].first = renumbered;
        order_.emplace(renumbered, path);
        ++index;
      }
      sequence_ = count + 1;
    }
    return Key{priority == TaskPriority::High, sequence_++};
  }
  std::map<QString, std::pair<Key, T>> tasks_;
  std::map<Key, QString> order_;
  quint64 sequence_ = 0;
};

void tryEmitEnd();

template <typename T> class WorkQueue {
public:
  bool enqueue(const QString &path, T data, TaskPriority priority) {
    std::unique_lock lock(mutex);
    const size_t capacity = priority == TaskPriority::High ? kMaxPendingMedia + kCatalogPage : kMaxPendingMedia;
    while (queue.size() >= capacity && !queue.contains(path)) {
      if (priority == TaskPriority::High) return false;
      cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (inProgress.contains(path)) return false;
    if (!queue.enqueue(path, std::move(data), priority)) return false;
    cv.notify_all();
    return true;
  }
  std::pair<QString, T> nextTaskBlocking(const std::function<size_t()> &limit) {
    std::unique_lock lock(mutex);
    for (;;) {
      if (!queue.empty() && size_t(inProgress.size()) < std::max<size_t>(limit(), 1)) {
        auto task = *queue.popFront();
        inProgress.insert(task.first);
        cv.notify_all();
        return task;
      }
      cv.wait(lock);
    }
  }
  void markDone(const QString &path, std::optional<T> retry = std::nullopt) {
    bool idle;
    {
      std::lock_guard lock(mutex);
      inProgress.remove(path);
      if (retry) queue.enqueue(path, std::move(*retry), TaskPriority::Low);
      idle = queue.empty() && inProgress.isEmpty();
    }
    cv.notify_all();
    if (idle) tryEmitEnd();
  }
  bool hasWork() {
    std::lock_guard lock(mutex);
    return !queue.empty() || !inProgress.isEmpty();
  }
  size_t outstanding() {
    std::lock_guard lock(mutex);
    return queue.size() + size_t(inProgress.size());
  }
  size_t trimPrefix(const QString &prefix) {
    std::lock_guard lock(mutex);
    const size_t removed = queue.trimPrefix(prefix);
    if (removed) cv.notify_all();
    return removed;
  }
  void clear() {
    std::lock_guard lock(mutex);
    queue.clear();
    cv.notify_all();
  }
  std::mutex mutex;
  std::condition_variable cv;
  PendingTasks<T> queue;
  QSet<QString> inProgress;
  size_t startedWorkers = 0;
};

struct ThumbTask {
  QString thumbDir;
};
struct Unit {};

// Never destroyed: detached workers wait on these, and glibc blocks destroying a condition variable with waiters.
WorkQueue<ThumbTask> &thumbWork = *new WorkQueue<ThumbTask>;
WorkQueue<Unit> &metaWork = *new WorkQueue<Unit>;

using Clock = std::chrono::steady_clock;
struct HashQueue {
  std::mutex mutex;
  std::condition_variable cv;
  PendingTasks<Unit> queue;
  QSet<QString> inProgress;
  QHash<QString, Clock::time_point> inProgressStarted;
  // Timed-out work may still be decoding on a worker. Keep its physical slot until that exact task returns.
  std::set<std::pair<QString, Clock::time_point>> runningTasks;
  size_t done = 0, startedWorkers = 0;
  size_t total() const { return done + queue.size() + size_t(inProgress.size()); }
};
HashQueue &hashQueue = *new HashQueue;

enum class Stage : quint8 { Conversion, Thumbnails, Metadata, Idle };
const char *stageName(Stage stage) {
  switch (stage) {
  case Stage::Conversion: return "conversion";
  case Stage::Thumbnails: return "thumbnails";
  case Stage::Metadata: return "metadata";
  default: return "idle";
  }
}

// Media of the albums the preloader has been given: a quick count when an album is queued, made exact once its pass
// has looked at every file.
struct AlbumPlan {
  size_t planned = 0, scanned = 0;
};
std::mutex planMutex;
QHash<QString, AlbumPlan> plan;

std::mutex queueMutex;
std::deque<QString> preloadQueue;
std::mutex preloadedMutex;
QHash<QString, QString> preloaded;
std::atomic<bool> preloaderRunning {false};
std::mutex cancelMutex;
std::optional<std::pair<QString, std::shared_ptr<std::atomic<bool>>>> currentCancel;
std::atomic<size_t> doneThumbs {0}, doneMeta {0}, conversionsTotal {0}, conversionsDone {0};
std::atomic<quint8> currentStageValue {quint8(Stage::Idle)};
std::mutex hashAlbumsMutex;
QSet<QString> thumbHashAlbums;
std::atomic<bool> eventsOn {false};
std::mutex emitMutex;
std::optional<Clock::time_point> lastEmit;
std::atomic<bool> allowOpen {false};
std::mutex rootMutex;
std::optional<QString> activeRoot;

std::optional<QString> root() {
  std::lock_guard lock(rootMutex);
  return activeRoot;
}
bool outsideRoot(const QString &path) {
  const auto r = root();
  return r && !path::startsWith(path, *r);
}

size_t quickMediaCount(const QString &dir) {
  try {
    size_t count = 0;
    for (const DirEntry &entry : readDir(dir))
      if (entry.isFile && constants::isMedia(entry.name)) ++count;
    return count;
  } catch (const Error &) {
    return 0;
  }
}
void planQueued() {
  std::vector<QString> queued;
  {
    std::lock_guard lock(queueMutex);
    queued.assign(preloadQueue.begin(), preloadQueue.end());
  }
  std::vector<QString> missing;
  {
    std::lock_guard lock(planMutex);
    for (const QString &dir : queued)
      if (!plan.contains(dir)) missing.push_back(dir);
  }
  if (missing.empty()) return;
  std::vector<std::pair<QString, size_t>> counted;
  for (const QString &dir : missing) counted.emplace_back(dir, quickMediaCount(dir));
  std::lock_guard lock(planMutex);
  for (const auto &[dir, planned] : counted)
    if (!plan.contains(dir)) plan.insert(dir, {planned, 0});
}
void planScanned(const QString &dir, size_t files) {
  std::lock_guard lock(planMutex);
  if (auto it = plan.find(dir); it != plan.end()) it->scanned += files;
}
void planSettle(const QString &dir) {
  std::lock_guard lock(planMutex);
  if (auto it = plan.find(dir); it != plan.end()) it->planned = it->scanned;
}
struct Totals {
  size_t planned = 0, scanned = 0;
  bool pending = false;
};
Totals planTotals() {
  std::lock_guard lock(planMutex);
  Totals t;
  for (const AlbumPlan &entry : plan) {
    t.planned += std::max(entry.planned, entry.scanned);
    t.scanned += entry.scanned;
  }
  t.pending = t.scanned < t.planned;
  return t;
}

std::pair<size_t, size_t> combinedTotals() {
  const size_t convTotal = conversionsTotal, convDone = conversionsDone;
  const size_t thumbs = doneThumbs, meta = doneMeta;
  const size_t completed = convDone + thumbs + meta;
  const size_t total = convTotal + thumbs + thumbWork.outstanding() + meta + metaWork.outstanding();
  return {completed, total};
}
size_t activeActions() {
  const size_t convTotal = conversionsTotal, convDone = conversionsDone;
  return (convTotal > convDone ? convTotal - convDone : 0) + thumbWork.outstanding() + metaWork.outstanding();
}
QJsonObject progressObject(size_t completed, size_t total) { return {{QStringLiteral("completed"), qint64(completed)}, {QStringLiteral("total"), qint64(total)}}; }
std::pair<size_t, size_t> plannedProgress(size_t done, size_t outstanding) {
  const Totals t = planTotals();
  // Work outside a preload pass, such as a preview asked for by the gallery.
  if (t.planned == 0) return {done, done + outstanding};
  const size_t completed = std::min(t.scanned > outstanding ? t.scanned - outstanding : 0, t.planned);
  return {completed, std::max(t.planned, completed + outstanding)};
}
std::pair<size_t, size_t> stageCounts(Stage stage) {
  switch (stage) {
  case Stage::Conversion: return {conversionsDone.load(), conversionsTotal.load()};
  case Stage::Thumbnails: return plannedProgress(doneThumbs, thumbWork.outstanding());
  case Stage::Metadata: return plannedProgress(doneMeta, metaWork.outstanding());
  default: return combinedTotals();
  }
}
Stage currentStage() {
  const size_t convTotal = conversionsTotal, convDone = conversionsDone;
  if (convTotal > convDone) return Stage::Conversion;
  if (thumbWork.outstanding() > 0 || (preloaderRunning && planTotals().pending)) return Stage::Thumbnails;
  if (metaWork.outstanding() > 0) return Stage::Metadata;
  return Stage::Idle;
}

void emitProgress(std::optional<Stage> requested) {
  const Stage previous = Stage(currentStageValue.load());
  const Stage stage = requested ? *requested : currentStage();
  currentStageValue = quint8(stage);
  const bool changed = stage != previous;
  const auto interval = std::chrono::milliseconds(readSettings().preload.progressEmitMs);
  const auto now = Clock::now();
  {
    std::lock_guard lock(emitMutex);
    if (!changed && lastEmit && now - *lastEmit < interval) return;
    lastEmit = now;
  }
  if (!eventsOn) return;
  const auto [completed, total] = combinedTotals();
  const auto [stageDone, stageTotal] = stageCounts(stage);
  const auto [convDone, convTotal] = stageCounts(Stage::Conversion);
  const auto [thumbDone, thumbTotal] = stageCounts(Stage::Thumbnails);
  const auto [metaDone, metaTotal] = stageCounts(Stage::Metadata);
  host::emitEvent(QStringLiteral("preload-progress"), QJsonObject{{QStringLiteral("stage"), QString::fromLatin1(stageName(stage))},
                                                                  {QStringLiteral("stage_progress"), progressObject(stageDone, stageTotal)},
                                                                  {QStringLiteral("overall_completed"), qint64(completed)},
                                                                  {QStringLiteral("overall_total"), qint64(total)},
                                                                  {QStringLiteral("progress"), qint64(total == 0 ? 0 : std::min<size_t>(completed * 100 / total, 100))},
                                                                  {QStringLiteral("conversions"), progressObject(convDone, convTotal)},
                                                                  {QStringLiteral("thumbnails"), progressObject(thumbDone, thumbTotal)},
                                                                  {QStringLiteral("metadata"), progressObject(metaDone, metaTotal)},
                                                                  {QStringLiteral("active_actions"), qint64(activeActions())}});
}

void emitHashEvent(size_t completed, size_t total) {
  if (eventsOn) host::emitEvent(QStringLiteral("hash-progress"), progressObject(completed, total));
}
void emitProcessed(const QString &file, const char *kind) {
  if (eventsOn) host::emitEvent(QStringLiteral("media-processed"), QJsonObject{{QStringLiteral("path"), file}, {QStringLiteral("kind"), QString::fromLatin1(kind)}});
}

std::chrono::seconds hashTaskTimeout(const AdvancedSettings &settings) {
  // Allow for a thumbnail lock holder, this task's FFmpeg attempt, and image decoding.
  return std::chrono::seconds(std::max<quint64>(settings.ffmpeg.timeoutSecs * 3 + 35, 30));
}

void resetPreloadState() {
  {
    std::lock_guard lock(cancelMutex);
    if (currentCancel) *currentCancel->second = true;
  }
  {
    std::lock_guard lock(queueMutex);
    preloadQueue.clear();
  }
  {
    std::lock_guard lock(preloadedMutex);
    preloaded.clear();
  }
  thumbWork.clear();
  metaWork.clear();
  {
    std::lock_guard lock(hashQueue.mutex);
    hashQueue.queue.clear();
    hashQueue.done = 0;
  }
  doneThumbs = doneMeta = conversionsTotal = conversionsDone = 0;
  {
    std::lock_guard lock(planMutex);
    plan.clear();
  }
  {
    std::lock_guard lock(emitMutex);
    lastEmit.reset();
  }
  {
    std::lock_guard lock(hashAlbumsMutex);
    thumbHashAlbums.clear();
  }
  currentStageValue = quint8(Stage::Idle);
  emitProgress(Stage::Idle);
}

void startThumbHashWorker();

bool hashEntryReady(const QString &file, const std::optional<FileMetaEntry> &entry, const HashConfig &config) {
  return entry && entry->hashVersion == config.hashVersion && entry->hashBits.value_or(config.bits) == config.bits && metadata::hashCacheCurrent(file, *entry) && (entry->hashFailed || entry->hash);
}

bool enqueueHashes(const QStringList &paths, const HashConfig &config, TaskPriority priority, bool emit) {
  if (paths.isEmpty()) return false;
  QStringList needed;
  for (const QString &file : paths) {
    std::optional<FileMetaEntry> entry;
    try {
      entry = metadata::readFileEntry(file);
    } catch (const Error &) {
    }
    if (!metadata::isImageMedia(path::parent(file), path::fileName(file))) continue;
    if (hashEntryReady(file, entry, config)) continue;
    needed << file;
  }
  if (needed.isEmpty()) return false;
  bool added = false;
  size_t done, total;
  {
    std::unique_lock lock(hashQueue.mutex);
    if (hashQueue.queue.empty() && hashQueue.inProgress.isEmpty() && hashQueue.done > 0) hashQueue.done = 0;
    for (const QString &file : needed) {
      while (hashQueue.queue.size() >= kMaxPendingMedia && !hashQueue.queue.contains(file)) {
        if (priority == TaskPriority::High) break;
        hashQueue.cv.wait_for(lock, std::chrono::milliseconds(100));
      }
      if (hashQueue.queue.size() >= kMaxPendingMedia && !hashQueue.queue.contains(file)) continue;
      if (hashQueue.inProgress.contains(file)) continue;
      added |= hashQueue.queue.enqueue(file, Unit{}, priority);
    }
    total = hashQueue.total();
    done = std::min(hashQueue.done, total);
    if (added) hashQueue.cv.notify_all();
  }
  if (added) {
    if (emit) emitHashEvent(done, total);
    startThumbHashWorker();
  }
  return added;
}

void scheduleThumbHashScan() {
  static std::atomic<bool> scanning {false};
  if (scanning.exchange(true)) return;
  std::thread([] {
    setLowPriorityCurrentThread();
    for (;;) {
      QString album;
      {
        std::lock_guard lock(hashAlbumsMutex);
        if (thumbHashAlbums.isEmpty()) break;
        album = *thumbHashAlbums.begin();
        thumbHashAlbums.remove(album);
      }
      const HashConfig config = readSettings().hashConfig();
      QStringList batch;
      QSet<QString> hidden;
      try {
        hidden = metadata::hiddenHeicNames(album);
      } catch (const Error &) {
      }
      try {
        for (const DirEntry &file : readDir(album)) {
          const auto r = root();
          if (!(r && path::startsWith(album, *r))) break;
          if (file.isFile && metadata::isImageMedia(album, file.name) && !hidden.contains(file.name)) {
            batch << path::join(album, file.name);
            if (batch.size() == 32) {
              enqueueHashes(batch, config, TaskPriority::Low, true);
              batch.clear();
            }
          }
        }
      } catch (const Error &) {
      }
      enqueueHashes(batch, config, TaskPriority::Low, true);
    }
    scanning = false;
  }).detach();
}

void tryEmitEnd() {
  const bool hashRunning = [] {
    std::lock_guard lock(hashQueue.mutex);
    return !hashQueue.queue.empty() || !hashQueue.inProgress.isEmpty();
  }();
  if (!preloaderRunning && !thumbWork.hasWork() && !metaWork.hasWork() && thumbWork.outstanding() == 0 && metaWork.outstanding() == 0) {
    doneThumbs = doneMeta = 0;
    {
      std::lock_guard lock(planMutex);
      plan.clear();
    }
    currentStageValue = quint8(Stage::Idle);
    emitProgress(Stage::Idle);
    if (!hashRunning) scheduleThumbHashScan();
  }
}

void queueAlbumForHashing(const QString &dir) {
  if (outsideRoot(dir)) return;
  std::lock_guard lock(hashAlbumsMutex);
  thumbHashAlbums.insert(dir);
}

void preloadDir(const QString &dir, const std::shared_ptr<std::atomic<bool>> &cancel) {
  if (!isDir(dir)) {
    qInfo("skip preload for missing album %s", qUtf8Printable(dir));
    return;
  }
  media_delete::recoverAlbum(dir);
  if (outsideRoot(dir)) return;
  const QString thumbDir = path::join(dir, thumb::kThumbDir);
  // One level only: an album deleted from outside must stay deleted.
  createDerivedDir(thumbDir);
  thumb::removeLegacyPreviewLocks(thumbDir);
  metadata::prepareDatabase(dir);
  album_db::FileReader reader(path::join(dir, QStringLiteral(".room237-metadata/album.sqlite")));
  const AdvancedSettings settings = readSettings();
  const QString version = settings.thumbVersion();
  const QSet<QString> hidden = metadata::hiddenHeicNames(dir);
  // Files are reported to the plan after their tasks are queued, so progress never runs ahead of the queues.
  size_t unreported = 0;
  for (const DirEntry &file : readDir(dir)) {
    if (*cancel) {
      planScanned(dir, unreported);
      return;
    }
    if (!file.isFile) continue;
    const QString filePath = path::join(dir, file.name);
    if (path::hasExtension(filePath, {"heic"}) && hidden.contains(file.name)) continue;
    if (!constants::isMedia(filePath)) continue;
    const std::optional<FileMetaEntry> cached = reader.get(file.name);
    if (!(cached && thumb::cachedPreviewsCurrent(filePath, thumbDir, *cached, version))) enqueueThumbTask(filePath, thumbDir, true, TaskPriority::Low);
    if (!(cached && metadata::metadataCacheCurrentWithSettings(filePath, *cached, settings))) enqueueMetaTask(filePath, true, TaskPriority::Low);
    if (++unreported == 64) {
      planScanned(dir, unreported);
      unreported = 0;
      emitProgress(std::nullopt);
    }
  }
  planScanned(dir, unreported);
}

void startThumbWorker() {
  const size_t desired = std::max<quint32>(readSettings().preload.thumbWorkers, 1);
  for (;;) {
    {
      std::lock_guard lock(thumbWork.mutex);
      if (thumbWork.startedWorkers >= desired) return;
      ++thumbWork.startedWorkers;
    }
    std::thread([] {
      setLowPriorityCurrentThread();
      metadata::BackgroundMetadataWrites background;
      for (;;) {
        auto [file, task] = thumbWork.nextTaskBlocking([] { return size_t(readSettings().preload.thumbWorkers); });
        const AdvancedSettings settings = readSettings();
        if (outsideRoot(file) || metadata::isThumbFailed(file)) {
          thumbWork.markDone(file);
          emitProgress(std::nullopt);
          continue;
        }
        const auto source = metadata::sourceRevision(file);
        try {
          thumb::ensureThumbWithSettings(file, task.thumbDir, settings);
          ++doneThumbs;
          emitProcessed(file, "thumbnail");
        } catch (const std::exception &e) {
          qWarning("Failed to generate thumbnail %s: %s", qUtf8Printable(file), e.what());
          if (source) {
            try {
              metadata::markThumbFailed(file, *source, settings.processingVersion());
            } catch (const std::exception &) {
            }
          }
        }
        const bool retry = settings.processingVersion() != readSettings().processingVersion();
        thumbWork.markDone(file, retry ? std::optional(task) : std::nullopt);
        emitProgress(Stage::Thumbnails);
      }
    }).detach();
  }
}

void startMetaWorker() {
  const size_t desired = std::max<quint32>(readSettings().preload.metaWorkers, 1);
  for (;;) {
    {
      std::lock_guard lock(metaWork.mutex);
      if (metaWork.startedWorkers >= desired) return;
      ++metaWork.startedWorkers;
    }
    std::thread([] {
      setLowPriorityCurrentThread();
      metadata::BackgroundMetadataWrites background;
      for (;;) {
        auto [file, unit] = metaWork.nextTaskBlocking([] { return size_t(readSettings().preload.metaWorkers); });
        if (outsideRoot(file) || metadata::isMetaFailed(file)) {
          metaWork.markDone(file);
          emitProgress(std::nullopt);
          continue;
        }
        const auto source = metadata::sourceRevision(file);
        const AdvancedSettings settings = readSettings();
        const QString version = settings.processingVersion();
        try {
          metadata::getFileMetadataCachedWithSettings(file, settings);
          ++doneMeta;
          emitProcessed(file, "metadata");
        } catch (const std::exception &e) {
          qWarning("Failed to extract metadata %s: %s", qUtf8Printable(file), e.what());
          if (source) {
            try {
              metadata::markMetaFailed(file, *source, version);
            } catch (const std::exception &) {
            }
          }
        }
        const bool retry = version != readSettings().processingVersion();
        metaWork.markDone(file, retry ? std::optional(Unit{}) : std::nullopt);
        emitProgress(Stage::Metadata);
      }
    }).detach();
  }
}

std::tuple<QString, Clock::time_point, AdvancedSettings> nextHashTask() {
  std::unique_lock lock(hashQueue.mutex);
  for (;;) {
    const AdvancedSettings settings = readSettings();
    if (!hashQueue.queue.empty() && hashQueue.runningTasks.size() < std::max<size_t>(settings.preload.hashWorkers, 1)) {
      const QString file = hashQueue.queue.popFront()->first;
      hashQueue.inProgress.insert(file);
      hashQueue.cv.notify_all();
      const auto started = Clock::now();
      hashQueue.runningTasks.emplace(file, started);
      hashQueue.inProgressStarted.insert(file, started);
      Q_UNUSED(hashTaskTimeout(settings));
      return {file, started, settings};
    }
    hashQueue.cv.wait(lock);
  }
}

std::pair<size_t, size_t> markHashDone(const QString &file, Clock::time_point started) {
  bool empty = false;
  size_t done, total;
  {
    std::lock_guard lock(hashQueue.mutex);
    hashQueue.runningTasks.erase({file, started});
    hashQueue.cv.notify_all();
    if (hashQueue.inProgressStarted.value(file) != started) {
      total = hashQueue.total();
      return {std::min(hashQueue.done, total), total};
    }
    hashQueue.inProgress.remove(file);
    hashQueue.inProgressStarted.remove(file);
    ++hashQueue.done;
    empty = hashQueue.queue.empty() && hashQueue.inProgress.isEmpty();
    total = hashQueue.total();
    done = std::min(hashQueue.done, total);
  }
  hashQueue.cv.notify_all();
  if (empty) tryEmitEnd();
  return {done, total};
}

void startThumbHashWorker() {
  const size_t desired = std::max<quint32>(readSettings().preload.hashWorkers, 1);
  for (;;) {
    {
      std::lock_guard lock(hashQueue.mutex);
      if (hashQueue.startedWorkers >= desired) return;
      ++hashQueue.startedWorkers;
    }
    std::thread([] {
      setLowPriorityCurrentThread();
      metadata::BackgroundMetadataWrites background;
      for (;;) {
        auto [original, started, settings] = nextHashTask();
        if (outsideRoot(original)) {
          markHashDone(original, started);
          continue;
        }
        const HashConfig config = settings.hashConfig();
        if (const auto cached = metadata::loadAlbumFileHash(original); cached && cached->version == config.hashVersion && cached->bits.value_or(config.bits) == config.bits) {
          const auto [done, total] = markHashDone(original, started);
          emitHashEvent(done, total);
          continue;
        }
        try {
          duplicates::computeHashForFile(original, config, settings);
        } catch (const std::exception &) {
        }
        const auto [done, total] = markHashDone(original, started);
        emitHashEvent(done, total);
        if (settings.preload.thumbHashQueueDelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(settings.preload.thumbHashQueueDelayMs));
      }
    }).detach();
  }
}
}  // namespace

bool artifactsMissing(const QString &dir) { return !exists(path::join(dir, thumb::kThumbDir)) || !exists(path::join(dir, metadata::kMetaDir)); }

bool setActiveRoot(const QString &newRoot) {
  {
    std::lock_guard lock(rootMutex);
    if (activeRoot && *activeRoot == newRoot) return false;
    activeRoot = newRoot;
  }
  resetPreloadState();
  return true;
}

bool enqueueThumbTask(const QString &file, const QString &thumbDir, bool, TaskPriority priority) {
  if (!thumbWork.enqueue(file, ThumbTask{thumbDir}, priority)) return false;
  emitProgress(std::nullopt);
  startThumbWorker();
  return true;
}

bool enqueueMetaTask(const QString &file, bool, TaskPriority priority) {
  if (!metaWork.enqueue(file, Unit{}, priority)) return false;
  emitProgress(std::nullopt);
  startMetaWorker();
  return true;
}

void enqueuePreload(const QString &dir) {
  if (!qaPathAllowed(dir) || outsideRoot(dir)) return;
  const bool missing = artifactsMissing(dir);
  const QString version = readSettings().processingVersion();
  {
    std::lock_guard lock(preloadedMutex);
    if (preloaded.value(dir) == version && !missing && preloaded.contains(dir)) return;
    preloaded.remove(dir);
  }
  {
    std::lock_guard lock(queueMutex);
    if (std::find(preloadQueue.begin(), preloadQueue.end(), dir) != preloadQueue.end()) return;
    preloadQueue.push_back(dir);
  }
  if (preloaderRunning) emitProgress(std::nullopt);
}

void enableEvents() { eventsOn = true; }

void startPreloaderWorker() {
  eventsOn = true;
  if (preloaderRunning.exchange(true)) return;
  {
    std::lock_guard lock(emitMutex);
    lastEmit.reset();
  }
  doneThumbs = doneMeta = 0;
  const Stage initial = currentStage();
  currentStageValue = quint8(initial);
  emitProgress(initial);
  std::thread([] {
    setLowPriorityCurrentThread();
    for (;;) {
      planQueued();
      std::optional<QString> next;
      {
        std::lock_guard lock(queueMutex);
        if (!preloadQueue.empty()) {
          next = preloadQueue.front();
          preloadQueue.pop_front();
        } else {
          preloaderRunning = false;
        }
      }
      if (!next) {
        tryEmitEnd();
        return;
      }
      const QString dir = *next;
      const QString version = readSettings().processingVersion();
      std::shared_ptr<std::atomic<bool>> cancel;
      {
        std::lock_guard rootLock(rootMutex);
        bool done;
        {
          std::lock_guard lock(preloadedMutex);
          done = preloaded.contains(dir) && preloaded.value(dir) == version;
        }
        if ((activeRoot && !path::startsWith(dir, *activeRoot)) || done) {
          std::lock_guard lock(planMutex);
          plan.remove(dir);
          continue;
        }
        cancel = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard lock(cancelMutex);
        currentCancel.emplace(dir, cancel);
      }
      queueAlbumForHashing(dir);
      QString error;
      try {
        preloadDir(dir, cancel);
      } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
      }
      planSettle(dir);
      {
        std::lock_guard lock(cancelMutex);
        currentCancel.reset();
      }
      // Checked under the lock that dropPreloadForPath takes after cancelling.
      std::lock_guard lock(preloadedMutex);
      if (*cancel) continue;
      if (error.isEmpty()) preloaded.insert(dir, version);
      else qWarning("Failed to preload %s: %s", qUtf8Printable(dir), qUtf8Printable(error));
    }
  }).detach();
}

void dropPreloadForPath(const QString &prefix) {
  // An in-flight pass must not mark the album done: its queued work is trimmed below.
  {
    std::lock_guard lock(cancelMutex);
    if (currentCancel && path::startsWith(currentCancel->first, prefix)) *currentCancel->second = true;
  }
  {
    std::lock_guard lock(queueMutex);
    preloadQueue.erase(std::remove_if(preloadQueue.begin(), preloadQueue.end(), [&](const QString &p) { return path::startsWith(p, prefix); }), preloadQueue.end());
  }
  {
    std::lock_guard lock(preloadedMutex);
    for (auto it = preloaded.begin(); it != preloaded.end();) it = path::startsWith(it.key(), prefix) ? preloaded.erase(it) : std::next(it);
  }
  {
    std::lock_guard lock(planMutex);
    for (auto it = plan.begin(); it != plan.end();) it = path::startsWith(it.key(), prefix) ? plan.erase(it) : std::next(it);
  }
  if (thumbWork.trimPrefix(prefix)) emitProgress(std::nullopt);
  if (metaWork.trimPrefix(prefix)) emitProgress(std::nullopt);
  std::optional<std::pair<size_t, size_t>> hashTotals;
  {
    std::lock_guard lock(hashQueue.mutex);
    if (hashQueue.queue.trimPrefix(prefix) > 0) {
      if (hashQueue.queue.empty() && hashQueue.inProgress.isEmpty()) hashQueue.done = 0;
      const size_t total = hashQueue.total();
      hashTotals.emplace(std::min(hashQueue.done, total), total);
      hashQueue.cv.notify_all();
    }
  }
  if (hashTotals) emitHashEvent(hashTotals->first, hashTotals->second);
  std::lock_guard lock(hashAlbumsMutex);
  for (auto it = thumbHashAlbums.begin(); it != thumbHashAlbums.end();) it = path::startsWith(*it, prefix) ? thumbHashAlbums.erase(it) : std::next(it);
}

bool isPreloading() { return preloaderRunning || thumbWork.outstanding() > 0 || metaWork.outstanding() > 0; }

void applyWorkerLimits() {
  // Wake parked workers on a limit increase even if no new path is enqueued.
  {
    std::lock_guard lock(thumbWork.mutex);
    thumbWork.cv.notify_all();
  }
  {
    std::lock_guard lock(metaWork.mutex);
    metaWork.cv.notify_all();
  }
  {
    std::lock_guard lock(hashQueue.mutex);
    hashQueue.cv.notify_all();
  }
  if (thumbWork.hasWork()) startThumbWorker();
  if (metaWork.hasWork()) startMetaWorker();
  bool hashWork;
  {
    std::lock_guard lock(hashQueue.mutex);
    hashWork = !hashQueue.queue.empty() || !hashQueue.inProgress.isEmpty();
  }
  if (hashWork) startThumbHashWorker();
}

bool lockUntilPreloaded() {
  if (isPreloading()) {
    eventsOn = true;
    while (isPreloading()) {
      emitProgress(std::nullopt);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  return true;
}

void setAllowOpen(bool allow) {
  eventsOn = true;
  allowOpen = allow;
  if (allow) {
    size_t done, total;
    {
      std::lock_guard lock(hashQueue.mutex);
      total = hashQueue.total();
      done = std::min(hashQueue.done, total);
    }
    if (total > 0) emitHashEvent(done, total);
  }
}

}  // namespace core::preload
