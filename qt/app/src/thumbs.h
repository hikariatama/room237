#pragma once
// Bounded thumbnail pipeline: a fixed worker pool, a replace-on-demand queue
// and a byte-budgeted LRU of finished device-pixel images.
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
struct ThumbRequest {
  QString key;       // cache key: thumb path + target size
  QString path;      // thumbnail file
  int size = 0;      // square device-pixel side; 0 keeps the file as decoded
  int height = 0;    // non-zero: a tile of size x height device pixels
  std::function<void(QImage &)> finish;  // Runs on the worker, so a frame draws one ready picture.
};
class ThumbPipeline : public QObject {
  Q_OBJECT
public:
  explicit ThumbPipeline(int workers, qint64 budgetBytes, QObject *parent = nullptr);
  ~ThumbPipeline() override;
  // Replaces queued work; the earliest entries are served first.
  void setDemand(std::vector<ThumbRequest> demand);
  const QImage *get(const QString &key);  // touches LRU
  bool has(const QString &key) const { return cache_.contains(key); }
  void forgetMissing(const QString &pathPrefix = {});
  void drop(const QString &key);  // The GPU cache now owns this image.
  void clear();
  void setBudget(qint64 bytes) { budget_ = bytes; trim(); }
  // Pre-decoded neighbours for the viewer and icons never compete with the visible demand queue.
  qint64 bytes() const { return bytes_; }
  int count() const { return cache_.size(); }
  qint64 decoded() const { return decoded_; }
signals:
  void ready(const QString &key);
  // A decode that started before forgetMissing() found no file; the views ask again instead of keeping it missing.
  void retry(const QString &key);
private:
  void work();
  void deliver(const QString &key, const QImage &image, bool missing, quint64 epoch);
  void trim();
  struct Entry { QImage image; qint64 touched = 0; };
  QHash<QString, Entry> cache_;
  qint64 clock_ = 0, bytes_ = 0, budget_, decoded_ = 0;
  QSet<QString> missing_;
  quint64 missingEpoch_ = 0;  // Bumped by forgetMissing(); read by workers under mutex_.
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<ThumbRequest> queue_;
  std::unordered_set<std::string> inFlight_;
  std::vector<std::thread> threads_;
  bool stopping_ = false;
};
QImage decodeThumbnail(const QString &path, int size, int height = 0);
QImage decodeOriginal(const QString &path, QSize *fullSize = nullptr);
// Decodes for a target box; large originals are downsampled by the decoder where it can.
QImage decodeForDisplay(const QString &path, QSize box, QSize *fullSize = nullptr);
void setFfmpegPath(const QString &path);
QImage decodeBlurPreview(const QString &dataUrl);
