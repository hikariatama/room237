#include "thumbs.h"
#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QImageIOHandler>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPainter>
#include "platform.h"
#include "qatrace.h"
#include <QColorSpace>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryFile>
#include <algorithm>
static QString gFfmpeg;
void setFfmpegPath(const QString &path) { gFfmpeg = path; }
static QImage cover(const QImage &source, int size) {
  if (source.isNull() || size <= 0) return source;
  const qreal scale = qMax(qreal(size) / source.width(), qreal(size) / source.height());
  const QSize scaled(qMax(size, qRound(source.width() * scale)), qMax(size, qRound(source.height() * scale)));
  QImage out = scale == 1.0 ? source : source.scaled(scaled, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  if (out.width() != size || out.height() != size) out = out.copy((out.width() - size) / 2, (out.height() - size) / 2, size, size);
  return out;
}
QImage decodeThumbnail(const QString &path, int size, int height) {
  QImageReader reader(path);
  reader.setAutoTransform(false);
  QImage image = reader.read();
  if (image.isNull()) return {};
  // Thumbnails can carry alpha (PNG, WebP, GIF); the tile background shows through the same as the original.
  if (image.hasAlphaChannel()) {
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  } else if (image.format() != QImage::Format_RGB32) {
    image = image.convertToFormat(QImage::Format_RGB32);
  }
  if (height > 0 && size > 0) {
    if (image.width() == size && image.height() == height) return image;
    return image.scaled(size, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }
  return cover(image, size);
}
// Formats without a Qt image plugin on this platform. Bounded: the process is killed after 30 seconds.
static QImage decodeThroughFfmpeg(const QString &path) {
  if (gFfmpeg.isEmpty()) return {};
  QProcess process;
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.start(gFfmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-i"), path, QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-c:v"), QStringLiteral("png"), QStringLiteral("-pix_fmt"), QStringLiteral("rgba"), QStringLiteral("-f"), QStringLiteral("image2pipe"), QStringLiteral("-")});
  if (!process.waitForStarted(5000)) return {};
  process.closeWriteChannel();
  QByteArray bytes;
  QElapsedTimer clock;
  clock.start();
  while (process.state() != QProcess::NotRunning && clock.elapsed() < 30000) {
    process.waitForReadyRead(200);
    bytes += process.readAllStandardOutput();
    if (bytes.size() > (1LL << 30)) break;
  }
  bytes += process.readAllStandardOutput();
  if (process.state() != QProcess::NotRunning) {
    process.kill();
    process.waitForFinished(1000);
    return {};
  }
  QImage image;
  image.loadFromData(bytes, "PNG");
  return image;
}
static bool needsNativeDecoder(const QString &path) {
  const QString ext = QFileInfo(path).suffix().toLower();
  return ext == QLatin1String("heic") || ext == QLatin1String("heif") || ext == QLatin1String("avif");
}
static QImage finished(QImage image) {
  if (image.isNull()) return image;
  if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb) image.convertToColorSpace(QColorSpace::SRgb);
  return image.hasAlphaChannel() ? image.convertToFormat(QImage::Format_ARGB32_Premultiplied) : image.convertToFormat(QImage::Format_RGB32);
}
QImage decodeForDisplay(const QString &path, QSize box, QSize *fullSize) {
  if (needsNativeDecoder(path)) {
    const int side = box.isValid() ? qMax(box.width(), box.height()) : 0;
    QImage image = Platform::decodeImageNative(path, side, fullSize);
    if (image.isNull()) {
      image = decodeThroughFfmpeg(path);
      if (fullSize) *fullSize = image.size();
      if (box.isValid() && (image.width() > box.width() || image.height() > box.height())) image = image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return finished(std::move(image));
  }
  QImageReader reader(path);
  reader.setAutoTransform(true);
  reader.setAllocationLimit(2048);
  QSize size = reader.size();
  const bool swapped = reader.transformation() & QImageIOHandler::TransformationRotate90;
  if (swapped) size.transpose();
  if (fullSize) *fullSize = size;
  // JPEG can downscale inside the decoder, which is the cheapest route to screen pixels.
  if (box.isValid() && size.isValid() && (size.width() > box.width() || size.height() > box.height())) {
    const QSize scaled = size.scaled(box, Qt::KeepAspectRatio);
    if (reader.supportsOption(QImageIOHandler::ScaledSize)) reader.setScaledSize(swapped ? scaled.transposed() : scaled);
  }
  QImage image = reader.read();
  if (image.isNull()) {
    image = decodeThroughFfmpeg(path);
    if (fullSize) *fullSize = image.size();
  }
  if (box.isValid() && (image.width() > box.width() || image.height() > box.height())) image = image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  return finished(std::move(image));
}
QImage decodeOriginal(const QString &path, QSize *fullSize) { return decodeForDisplay(path, QSize(), fullSize); }
QImage decodeBlurPreview(const QString &dataUrl) {
  const qsizetype comma = dataUrl.indexOf(QLatin1Char(','));
  QByteArray bytes = QByteArray::fromBase64((comma >= 0 ? dataUrl.mid(comma + 1) : dataUrl).toLatin1());
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, "webp");
  return reader.read().convertToFormat(QImage::Format_RGB32);
}
ThumbPipeline::ThumbPipeline(int workers, qint64 budgetBytes, QObject *parent) : QObject(parent), budget_(budgetBytes) {
  for (int i = 0; i < workers; i++) threads_.emplace_back([this] { work(); });
}
ThumbPipeline::~ThumbPipeline() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    queue_.clear();
  }
  wake_.notify_all();
  for (auto &t : threads_) t.join();
}
void ThumbPipeline::setDemand(std::vector<ThumbRequest> demand) {
  std::deque<ThumbRequest> next;
  for (auto &r : demand)
    if (!cache_.contains(r.key) && !missing_.contains(r.key)) next.push_back(std::move(r));
  {
    std::lock_guard lock(mutex_);
    queue_ = std::move(next);
  }
  wake_.notify_all();
}
const QImage *ThumbPipeline::get(const QString &key) {
  auto it = cache_.find(key);
  if (it == cache_.end()) return nullptr;
  it->touched = ++clock_;
  return &it->image;
}
void ThumbPipeline::forgetMissing(const QString &) {
  {
    std::lock_guard lock(mutex_);
    missing_.clear();
    missingEpoch_++;
  }
  qaTrace("thumb-forget");
}
void ThumbPipeline::drop(const QString &key) {
  auto it = cache_.find(key);
  if (it == cache_.end()) return;
  bytes_ -= it->image.sizeInBytes();
  cache_.erase(it);
}
void ThumbPipeline::clear() {
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    missingEpoch_++;
  }
  cache_.clear();
  missing_.clear();
  bytes_ = 0;
}
void ThumbPipeline::work() {
  Platform::lowerThreadPriority();
  for (;;) {
    ThumbRequest request;
    quint64 epoch = 0;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      while (!queue_.empty()) {
        request = std::move(queue_.front());
        queue_.pop_front();
        if (inFlight_.insert(request.key.toStdString()).second) break;
        request.key.clear();
      }
      if (request.key.isEmpty()) continue;
      epoch = missingEpoch_;
    }
    QImage image = decodeThumbnail(request.path, request.size, request.height);
    const bool isMissing = image.isNull();
    if (!isMissing && request.finish) request.finish(image);
    {
      std::lock_guard lock(mutex_);
      inFlight_.erase(request.key.toStdString());
    }
    QMetaObject::invokeMethod(this, [this, key = request.key, image, isMissing, epoch] { deliver(key, image, isMissing, epoch); }, Qt::QueuedConnection);
  }
}
void ThumbPipeline::deliver(const QString &key, const QImage &image, bool isMissing, quint64 epoch) {
  if (isMissing) {
    bool stale;
    {
      std::lock_guard lock(mutex_);
      stale = epoch != missingEpoch_;
      if (!stale) missing_.insert(key);
    }
    qaTrace(stale ? "thumb-missing-stale" : "thumb-missing", key);
    // The thumbnail may have been written after this decode began and after the refresh that cleared misses.
    if (stale) emit retry(key);
    return;
  }
  decoded_++;
  bytes_ += image.sizeInBytes();
  auto old = cache_.find(key);
  if (old != cache_.end()) bytes_ -= old->image.sizeInBytes();
  cache_.insert(key, Entry{image, ++clock_});
  trim();
  emit ready(key);
}
void ThumbPipeline::trim() {
  if (bytes_ <= budget_) return;
  std::vector<std::pair<qint64, QString>> order;
  order.reserve(cache_.size());
  for (auto it = cache_.cbegin(); it != cache_.cend(); ++it) order.emplace_back(it->touched, it.key());
  std::sort(order.begin(), order.end());
  for (const auto &[touched, key] : order) {
    if (bytes_ <= budget_ * 8 / 10) break;
    bytes_ -= cache_.value(key).image.sizeInBytes();
    cache_.remove(key);
  }
}
