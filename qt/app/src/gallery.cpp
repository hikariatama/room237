#include "gallery.h"
#include "widgets.h"
#include "i18n.h"
#include "theme.h"
#include <QApplication>
#include <QDateTime>
#include <QVarLengthArray>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLocale>
#include <QMimeData>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QUrl>
#include <QWheelEvent>
#include <cmath>
#include <time.h>
#ifdef Q_OS_MACOS
double uptimeMs() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
#else
double uptimeMs() {
  static QElapsedTimer clock = [] { QElapsedTimer t; t.start(); return t; }();
  return clock.nsecsElapsed() / 1e6;
}
#endif
qint64 envMegabytes(const char *name, qint64 fallback) {
  bool ok = false;
  const qint64 value = qEnvironmentVariableIntValue(name, &ok);
  return (ok ? value : fallback) * 1024 * 1024;
}
QRect scrollThumbRect(int w, int h, int value, int maximum) {
  if (maximum <= 0) return {};
  const int length = qMax(32, int(qint64(h) * h / (maximum + h)));
  const int y = int(double(value) / maximum * (h - length));
  return {w - 19, y, 8, length};  // The scroll bar sits inside the viewport padding.
}
void paintScrollThumb(QPainter &painter, const QRect &thumb, qreal alpha) {
  if (thumb.isNull() || alpha <= 0) return;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  QColor color = Theme::border;
  color.setAlphaF(color.alphaF() * alpha);
  painter.setBrush(color);
  painter.drawRoundedRect(thumb, 4, 4);
  painter.restore();
}
bool envFlag(const char *name, bool fallback) { return qEnvironmentVariableIsSet(name) ? qEnvironmentVariableIntValue(name) != 0 : fallback; }
QString formatMediaDate(double seconds, bool compact) {
  if (seconds <= 0) return t("media.unknownDate");
  const QDateTime when = QDateTime::fromSecsSinceEpoch(qint64(seconds));
  const QLocale locale(I18n::language() == QLatin1String("ru") ? QLocale::Russian : QLocale::English);
  return locale.toString(when.date(), compact ? QStringLiteral("dd.MM.yy") : QStringLiteral("d MMM yyyy"));
}
static const char *kInternalMime = "application/x-room237";
GalleryView::GalleryView(GalleryModel *model, ThumbPipeline *pipeline, QWidget *parent) : QAbstractScrollArea(parent), model_(model), pipeline_(pipeline) {
  setFrameShape(QFrame::NoFrame);
  setFocusPolicy(Qt::StrongFocus);
  setViewportMargins(4, 4, 0, 0);  // The duplicates list uses the same margins.
  viewport()->setAcceptDrops(true);
  setAcceptDrops(true);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  viewport()->setAttribute(Qt::WA_NoSystemBackground);
  viewport()->setMouseTracking(true);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setAccessibleName(t("media.selectAlbum"));
  frameCap_.setSingleShot(true);
  connect(&frameCap_, &QTimer::timeout, this, [this] {
    if (framePending_) {
      framePending_ = false;
      viewport()->update();
      frameCap_.start(capMs_);
    }
  });
  setFrameCap(qEnvironmentVariableIntValue("ROOM237_FRAME_CAP_MS"));
  demandTimer_.setSingleShot(true);
  demandTimer_.setInterval(0);
  connect(&demandTimer_, &QTimer::timeout, this, &GalleryView::updateDemand);
  connect(pipeline_, &ThumbPipeline::retry, this, [this] { if (isVisible()) demandTimer_.start(); });
  // Prefetched rows need no repaint; only tiles on screen change the picture.
  connect(pipeline_, &ThumbPipeline::ready, this, [this] {
    if (!isVisible()) return;
    const int ready = visibleState().ready;
    if (ready != visibleReadyPainted_) {
      visibleReadyPainted_ = ready;
      viewport()->update();
    }
  });
  connect(model_, &GalleryModel::reset, this, [this] {
    all_ = false;
    selected_.clear();
    excluded_.clear();
    hovered_ = confirm_ = -1;
    springs_.clear();
    hoverHit_ = HitNone;
    relayout();
    emit selectionChanged();
  });
  connect(model_, &GalleryModel::pageLoaded, this, [this] {
    viewport()->update();
    demandTimer_.start();
  });
  connect(model_, &GalleryModel::geometryChanged, this, [this] {
    if (nonUniform()) {
      relayout();
      restoreAnchor();
    }
  });
  sampler_.setInterval(100);
  connect(&sampler_, &QTimer::timeout, this, [this] {
    QVector<QPair<QString, bool>> keys;
    const Visible v = visibleState(&keys);
    const double now = recordClock_.nsecsElapsed() / 1e6;
    rec_.samples.push_back({now, v.visible, v.ready, v.expected, v.blank, double(verticalScrollBar()->value())});
    for (const auto &[key, ok] : keys) {
      if (!rec_.first.contains(key)) rec_.first.insert(key, now);
      if (ok && !rec_.readyAt.contains(key)) rec_.readyAt.insert(key, now);
    }
  });
  springTick_.setTimerType(Qt::PreciseTimer);
  springTick_.setInterval(8);
  connect(&springTick_, &QTimer::timeout, this, &GalleryView::stepSprings);
  springClock_.start();
  hoverFade_.setDuration(150);
  hoverFade_.setStartValue(0.0);
  hoverFade_.setEndValue(1.0);
  connect(&hoverFade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    hoverAlpha_ = v.toReal();
    if (hovered_ >= 0) viewport()->update(tileRect(hovered_));
  });
  indicatorFade_.setDuration(200);
  connect(&indicatorFade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    indicatorAlpha_ = v.toReal();
    viewport()->update(QRect(viewport()->width() - 19, 0, 9, viewport()->height()));
  });
  indicatorHide_.setSingleShot(true);
  indicatorHide_.setInterval(900);
  connect(&indicatorHide_, &QTimer::timeout, this, [this] {
    if (underMouse()) return;  // The bar stays while the pointer is over the list.
    indicatorFade_.stop();
    indicatorFade_.setStartValue(indicatorAlpha_);
    indicatorFade_.setEndValue(0.0);
    indicatorFade_.start();
  });
  scrollIdle_.setSingleShot(true);
  scrollIdle_.setInterval(120);
  connect(&scrollIdle_, &QTimer::timeout, this, [this] { scrolling_ = false; });
  smoothScroll_.setDuration(160);
  smoothScroll_.setEasingCurve(QEasingCurve::OutCubic);
  connect(&smoothScroll_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { verticalScrollBar()->setValue(qRound(v.toReal())); });
  connect(&smoothScroll_, &QVariantAnimation::finished, this, [this] { wheelTarget_ = -1; });
}
double GalleryView::gap() const { return (1.0 - columns_ / 12.0) * 8 + 8; }
// The right 20 points hold the scroll bar; tiles never run under it.
int GalleryView::layoutWidth() const { return qMax(1, viewport()->width() - 20); }
double GalleryView::tilePoints() const { return qMax(1.0, (layoutWidth() - gap() * (columns_ - 1)) / columns_); }
double GalleryView::step() const { return tilePoints() + gap(); }
int GalleryView::devicePixels() const { return qRound((qRound(tilePoints()) - 2) * devicePixelRatioF()); }
bool GalleryView::nonUniform() const { return layout_ != Grid || model_->favoritesAlbum; }
// A layout or column change keeps the first visible tile where it was, whichever layout comes next.
void GalleryView::captureAnchor() {
  anchorIndex_ = -1;
  if (verticalScrollBar()->value() <= 0 || model_->total() <= 0) return;
  const auto [first, last] = visibleRange();
  for (int index = first; index <= last; index++) {
    const QRect r = tileRect(index);
    if (r.isNull() || r.bottom() < 0) continue;
    anchorIndex_ = index;
    anchorOffset_ = r.top();
    return;
  }
}
void GalleryView::restoreAnchor() {
  if (anchorIndex_ < 0 || (nonUniform() && !model_->geometryReady())) return;
  const QRect r = tileRect(anchorIndex_);
  if (r.isNull()) return;
  verticalScrollBar()->setValue(verticalScrollBar()->value() + r.top() - anchorOffset_);
  anchorIndex_ = -1;
  viewport()->update();
  demandTimer_.start();
}
void GalleryView::setColumns(int columns) {
  columns = qBound(1, columns, 12);
  if (columns != columns_) captureAnchor();
  columns_ = columns;
  relayout();
  restoreAnchor();
}
void GalleryView::setLayoutKind(Layout layout) {
  if (layout == layout_) return;
  captureAnchor();
  layout_ = layout;
  relayout();
  restoreAnchor();
}
// Rectangles are in content coordinates.
void GalleryView::rebuildGeometry() {
  rects_.clear();
  groups_.clear();
  maxTileHeight_ = 0;
  contentHeight_ = 0;
  if (!nonUniform()) return;
  if (!model_->geometryReady()) {
    model_->ensureGeometry();
    return;
  }
  const QVector<GeometryItem> &items = model_->geometry();
  const qreal width = layoutWidth(), g = gap();
  // Every layout uses the grid's columns, so their edges line up with the header and with each other.
  const qreal columnWidth = qMax<qreal>(1, (width - g * (columns_ - 1)) / columns_);
  const bool grouped = model_->favoritesAlbum;
  rects_.reserve(items.size());
  QVector<qreal> lanes(columns_, 0);
  qreal rowY = 0, rowHeight = 0;
  int column = 0;
  QVector<int> pending;
  QString currentAlbum;
  bool first = true;
  const auto finishRow = [&] {
    if (layout_ == Apple)
      for (int index : pending) rects_[index].moveTop(rowY + (rowHeight - rects_[index].height()) / 2);
    pending.clear();
  };
  for (int i = 0; i < items.size(); i++) {
    const GeometryItem &item = items[i];
    if (grouped && (first || item.albumId != currentAlbum)) {
      finishRow();
      qreal y = rowY + rowHeight;
      for (qreal lane : lanes) y = qMax(y, lane);
      if (!groups_.isEmpty()) groups_.back().end = y;
      groups_.push_back({item.albumName, y, 0});
      currentAlbum = item.albumId;
      first = false;
      lanes.fill(y + 44);
      rowY = y + 44;
      rowHeight = 0;
      column = 0;
    }
    const qreal ratio = qreal(item.height) / qMax(1, item.width);
    QRectF rect;
    if (layout_ == Masonry) {
      int lane = 0;
      for (int n = 1; n < columns_; n++)
        if (lanes[n] < lanes[lane]) lane = n;
      const qreal height = qMax<qreal>(2, (columnWidth - 2) * ratio + 2);
      rect = QRectF(lane * (columnWidth + g), lanes[lane], columnWidth, height);
      lanes[lane] += height + g;
    } else {
      const qreal native = qMin<qreal>(item.width, 450.0 / qMax<qreal>(0.001, ratio));
      const qreal tile = layout_ == Apple ? qMin(columnWidth, native + 2) : columnWidth;
      const qreal height = layout_ == Grid ? columnWidth : qMax<qreal>(2, (tile - 2) * ratio + 2);
      rect = QRectF(column * (columnWidth + g) + (columnWidth - tile) / 2, rowY, tile, height);
      rowHeight = qMax(rowHeight, height);
      pending.push_back(i);
      column++;
      if (column == columns_) {
        rects_.push_back(rect);
        finishRow();
        rowY += rowHeight + g;
        rowHeight = 0;
        column = 0;
        maxTileHeight_ = qMax(maxTileHeight_, rect.height());
        continue;
      }
    }
    rects_.push_back(rect);
    maxTileHeight_ = qMax(maxTileHeight_, rect.height());
  }
  finishRow();
  contentHeight_ = rowY + rowHeight;
  for (qreal lane : lanes) contentHeight_ = qMax(contentHeight_, lane);
  if (!groups_.isEmpty()) groups_.back().end = contentHeight_;
}
void GalleryView::relayout() {
  visibleReadyPainted_ = -1;
  rebuildGeometry();
  int height;
  if (nonUniform()) height = qCeil(contentHeight_);
  else {
    const int rows = (model_->total() + columns_ - 1) / columns_;
    height = int(rows * step() - gap());
  }
  verticalScrollBar()->setPageStep(viewport()->height());
  verticalScrollBar()->setSingleStep(40);
  verticalScrollBar()->setRange(0, qMax(0, height - viewport()->height()));
  viewport()->update();
  demandTimer_.start();
}
void GalleryView::rebuildChrome() {
  // The scroll fades are pictures, so a scrolling frame only copies them.
  const qreal ratio = viewport()->devicePixelRatioF();
  const auto fade = [&](bool top) {
    QPixmap out(QSize(layoutWidth(), 48) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    QPainter p(&out);
    QLinearGradient gradient(0, top ? 0 : 48, 0, top ? 48 : 0);
    gradient.setColorAt(0, Theme::sidebar);
    gradient.setColorAt(1, QColor(0x15, 0x14, 0x14, 0));
    p.fillRect(QRect(0, 0, layoutWidth(), 48), gradient);
    return out;
  };
  topFade_ = fade(true);
  bottomFade_ = fade(false);
}
void GalleryView::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  rebuildChrome();
  relayout();
}
void GalleryView::scrollContentsBy(int, int) {
  if (capMs_ > 0) {
    if (frameCap_.isActive()) {
      framePending_ = true;
    } else {
      viewport()->update();
      frameCap_.start(capMs_);
    }
  } else {
    viewport()->update();
  }
  demandTimer_.start();
  scrolling_ = true;
  scrollIdle_.start();
  setTileTarget(hovered_, 1);
  hovered_ = -1;
  hoverHit_ = HitNone;
  if (indicatorAlpha_ < 1 && indicatorFade_.state() != QAbstractAnimation::Running) {
    indicatorAlpha_ = 1;
  }
  indicatorHide_.start();
}
void GalleryView::setFrameCap(int milliseconds) { capMs_ = qMax(0, milliseconds); }
QString GalleryView::cacheKey(const MediaItem &item, const QSize &px) const {
  return item.path() + QLatin1Char('#') + QString::number(px.width()) + (px.height() == px.width() ? QString() : QLatin1Char('x') + QString::number(px.height()));
}
QSize GalleryView::deviceSizeFor(int index) const {
  if (nonUniform() && index < rects_.size()) {
    const QRect r = tileRect(index);
    const qreal ratio = devicePixelRatioF();
    return QSize(qRound((r.width() - 2) * ratio), qRound((r.height() - 2) * ratio));
  }
  const int px = devicePixels();
  return QSize(px, px);
}
qreal GalleryView::tileScale(int index) const {
  const auto it = springs_.constFind(index);
  return it == springs_.cend() ? 1 : it->value;
}
QRect GalleryView::springRect(int index) const {
  const QRect rect = tileRect(index);
  const int margin = qCeil(qMax(rect.width(), rect.height()) * 0.04) + 2;
  return rect.adjusted(-margin, -margin, margin, margin);
}
void GalleryView::setTileTarget(int index, qreal target) {
  if (index < 0) return;
  auto it = springs_.find(index);
  if (it == springs_.end()) {
    if (qFuzzyCompare(target, 1)) return;
    it = springs_.insert(index, Spring{});
  }
  it->target = target;
  if (Theme::reduceMotion()) {
    it->value = target;
    it->velocity = 0;
    viewport()->update(springRect(index));
    if (qFuzzyCompare(target, 1)) springs_.erase(it);
    return;
  }
  if (!springTick_.isActive()) {
    springLast_ = springClock_.elapsed();
    springTick_.start();
  }
}
void GalleryView::setHoverHit(Hit hit) {
  if (hit == hoverHit_) return;
  if (hoverHit_ != HitNone && hovered_ >= 0) viewport()->update(springRect(hovered_));
  hoverHit_ = hit;
  hitSpring_ = Spring{};
  hitSpring_.target = hit == HitNone ? 1 : 1.1;
  if (Theme::reduceMotion()) hitSpring_.value = hitSpring_.target;
  if (!springTick_.isActive()) {
    springLast_ = springClock_.elapsed();
    springTick_.start();
  }
}
void GalleryView::stepSprings() {
  const qint64 now = springClock_.elapsed();
  const qreal seconds = qMin<qint64>(now - springLast_, 50) / 1000.0;
  springLast_ = now;
  bool moving = false;
  for (auto it = springs_.begin(); it != springs_.end();) {
    const bool still = it->step(seconds);
    viewport()->update(springRect(it.key()));
    if (!still && qFuzzyCompare(it->target, 1)) it = springs_.erase(it);
    else {
      moving |= still;
      ++it;
    }
  }
  if (hitSpring_.step(seconds)) {
    moving = true;
    if (hovered_ >= 0) viewport()->update(springRect(hovered_));
  }
  if (!moving) springTick_.stop();
}
QRect GalleryView::tileRect(int index) const {
  if (nonUniform()) {
    if (index < 0 || index >= rects_.size()) return {};
    return rects_[index].translated(0, -verticalScrollBar()->value()).toRect();
  }
  const double s = step();
  const int row = index / columns_, col = index % columns_;
  const int y = qRound(row * s) - verticalScrollBar()->value();
  const int x = qRound(col * s);
  const int size = qRound(tilePoints());
  return {x, y, size, size};
}
std::pair<int, int> GalleryView::visibleRange(int extra) const {
  const int total = model_->total();
  const int top = verticalScrollBar()->value() - extra, bottom = verticalScrollBar()->value() + viewport()->height() + extra;
  if (total <= 0) return {0, -1};
  if (nonUniform()) {
    if (rects_.isEmpty()) return {0, -1};
    // Tiles are placed in non-decreasing y order; the tallest tile bounds how far back one can reach.
    auto lower = std::lower_bound(rects_.cbegin(), rects_.cend(), top - maxTileHeight_, [](const QRectF &r, qreal y) { return r.top() < y; });
    auto upper = std::upper_bound(rects_.cbegin(), rects_.cend(), qreal(bottom), [](qreal y, const QRectF &r) { return y < r.top(); });
    return {int(lower - rects_.cbegin()), int(upper - rects_.cbegin()) - 1};
  }
  const double s = step();
  const int firstRow = qMax(0, int(top / s)), lastRow = int(bottom / s);
  return {firstRow * columns_, qMin(total - 1, (lastRow + 1) * columns_ - 1)};
}
int GalleryView::indexAt(QPoint p) const {
  if (nonUniform()) {
    const auto [first, last] = visibleRange();
    for (int i = first; i <= last; i++)
      if (tileRect(i).contains(p)) return i;
    return -1;
  }
  const double s = step();
  const int y = p.y() + verticalScrollBar()->value();
  const int row = int(y / s), col = int(p.x() / s);
  if (col < 0 || col >= columns_ || p.x() - col * s > tilePoints() || y - row * s > tilePoints()) return -1;
  const int index = row * columns_ + col;
  return index < model_->total() ? index : -1;
}
// Corner radius in px: (1 - columns / 12) * 0.75rem + 0.15rem.
int GalleryView::tileRadius() const { return qRound((1.0 - columns_ / 12.0) * 12 + 2.4); }
void GalleryView::paintCorners(QPainter &painter, const QRect &rect) {
  const qreal ratio = viewport()->devicePixelRatioF();
  const int r = tileRadius();
  if (ratio != cornersRatio_ || r != cornersRadius_) {
    cornersRatio_ = ratio;
    cornersRadius_ = r;
    const int side = qCeil(r * ratio);
    // The carpet varies by a few levels; its average is invisible in a corner this small and keeps tiles opaque blits.
    const QColor page(21, 20, 20);
    QPixmap base(side, side);
    base.fill(Qt::transparent);
    {
      QPainter p(&base);
      p.setRenderHint(QPainter::Antialiasing);
      QPainterPath path;
      path.addRect(0, 0, side, side);
      QPainterPath round;
      round.addEllipse(0, 0, side * 2, side * 2);
      p.fillPath(path.subtracted(round), page);
      p.setPen(QPen(QColor(255, 255, 255, 13), ratio));
      p.setBrush(Qt::NoBrush);
      p.drawEllipse(QRectF(ratio / 2, ratio / 2, side * 2 - ratio, side * 2 - ratio));
    }
    const QImage image = base.toImage();
    corners_[0] = base;
    corners_[1] = QPixmap::fromImage(image.flipped(Qt::Horizontal));
    corners_[2] = QPixmap::fromImage(image.flipped(Qt::Vertical));
    corners_[3] = QPixmap::fromImage(image.flipped(Qt::Horizontal | Qt::Vertical));
    for (QPixmap &corner : corners_) corner.setDevicePixelRatio(ratio);
  }
  const qreal size = corners_[0].width() / ratio;
  painter.drawPixmap(QPointF(rect.left(), rect.top()), corners_[0]);
  painter.drawPixmap(QPointF(rect.right() + 1 - size, rect.top()), corners_[1]);
  painter.drawPixmap(QPointF(rect.left(), rect.bottom() + 1 - size), corners_[2]);
  painter.drawPixmap(QPointF(rect.right() + 1 - size, rect.bottom() + 1 - size), corners_[3]);
}
void GalleryView::paintDateBadge(QPainter &painter, const QRect &rect, const MediaItem &item) {
  // Scaled down when it would meet the heart.
  const QString date = formatMediaDate(item.displayDate(), columns_ >= 9);
  painter.save();
  painter.setOpacity(hoverAlpha_);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setFont(Theme::font(12));
  const int textWidth = painter.fontMetrics().horizontalAdvance(date);
  const int available = rect.width() - 24 - 18;
  const qreal scale = available > 0 ? qMin<qreal>(1, qreal(available) / (textWidth + 12)) : 0;
  if (scale > 0) {
    painter.translate(rect.left() + 8, rect.top() + 8);
    painter.scale(scale, scale);
    const QRect badge(0, 0, textWidth + 12, 19);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 140));
    painter.drawRoundedRect(badge, Theme::kRadiusMd, Theme::kRadiusMd);
    painter.setPen(Qt::white);
    painter.drawText(badge, Qt::AlignCenter, date);
  }
  painter.restore();
}
// Border and backdrop already blended over the page, so the edges stay opaque blits.
static const QColor kTileEdge(34, 33, 33);
const QImage *GalleryView::pictureFor(int index) {
  const MediaItem *item = model_->item(index);
  if (!item) return nullptr;
  const QImage *picture = pipeline_->get(cacheKey(*item, deviceSizeFor(index)));
  return picture ? picture : model_->blur(*item);
}
void GalleryView::paintTile(QPainter &painter, const QRect &rect, int index, const QImage *picture) {
  const MediaItem *item = model_->item(index);
  painter.fillRect(rect.x(), rect.y(), rect.width(), 1, kTileEdge);
  painter.fillRect(rect.x(), rect.bottom(), rect.width(), 1, kTileEdge);
  painter.fillRect(rect.x(), rect.y() + 1, 1, rect.height() - 2, kTileEdge);
  painter.fillRect(rect.right(), rect.y() + 1, 1, rect.height() - 2, kTileEdge);
  const QRect inner = rect.adjusted(1, 1, -1, -1);
  if (!picture || picture->hasAlphaChannel()) painter.fillRect(inner, QColor(23, 23, 23, 102));
  if (picture) painter.drawImage(inner, *picture);
  paintCorners(painter, rect);
  if (!item) return;
  const qreal ratio = viewport()->devicePixelRatioF();
  const bool selected = isSelected(index);
  if (item->isVideo) {
    static QHash<int, QPixmap> badges;
    const int key = int(ratio * 10);
    auto it = badges.constFind(key);
    if (it == badges.cend()) {
      const int d = 36;
      QPixmap badge(QSize(d, d) * ratio);
      badge.fill(Qt::transparent);
      badge.setDevicePixelRatio(ratio);
      QPainter b(&badge);
      b.setRenderHint(QPainter::Antialiasing);
      b.setPen(QPen(QColor(255, 255, 255, 38), 1));
      b.setBrush(QColor(0, 0, 0, 115));
      b.drawEllipse(QRectF(0.5, 0.5, d - 1, d - 1));
      Theme::drawIcon(b, "player-play-filled", QRectF(11, 10, 16, 16), QColor(255, 255, 255, 230));
      b.end();
      it = badges.insert(key, badge);
    }
    painter.drawPixmap(rect.center().x() - 18, rect.center().y() - 18, *it);
  }
  if (item->favorite && index != hovered_) {
    const int size = columns_ > 10 ? 12 : 14;
    const int inset = columns_ > 10 ? 10 : 8;
    Theme::drawIcon(painter, "heart-filled", QRectF(rect.right() + 1 - inset - size, rect.top() + inset, size, size), Theme::red500);
  }
  if (selected) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Theme::blue500, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect).adjusted(1, 1, -1, -1), tileRadius() - 1, tileRadius() - 1);
    const QRectF check(rect.left() + 6, rect.bottom() + 1 - 26, 20, 20);
    painter.setPen(QPen(QColor(255, 255, 255, 204), 2));
    painter.setBrush(Theme::blue500);
    painter.drawEllipse(check.adjusted(-1, -1, 1, 1));
    Theme::drawIcon(painter, "check", check.adjusted(4, 4, -4, -4), Qt::white);
    painter.restore();
  }
  if (index == focus_ && hasFocus() && focusVisible() && !selected) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(255, 255, 255, 230), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect).adjusted(1, 1, -1, -1), tileRadius() - 1, tileRadius() - 1);
    painter.restore();
  }
}
QRect GalleryView::actionRect(const QRect &tile, Hit hit) const {
  const int right = tile.right() + 1, bottom = tile.bottom() + 1;
  const bool wide = columns_ <= 6;
  switch (hit) {
    case HitFavorite: return {right - 4 - 24, tile.top() + 4, 24, 24};
    case HitCopy: return {right - 6 - 28 - 4 - 28, bottom - 6 - 28, 28, 28};
    case HitDelete: return {right - 6 - 28, bottom - 6 - 28, 28, 28};
    case HitConfirmDelete: return wide ? QRect(tile.center().x() - 74, tile.center().y() + 2, 80, 26) : QRect(tile.center().x() - 26, tile.center().y() - 12, 26, 24);
    case HitConfirmCancel: return wide ? QRect(tile.center().x() + 10, tile.center().y() + 2, 62, 26) : QRect(tile.center().x() + 4, tile.center().y() - 12, 24, 24);
    default: return {};
  }
}
GalleryView::Hit GalleryView::hitAt(int index, QPoint position) const {
  if (index < 0 || columns_ > 10) return HitNone;
  const QRect tile = tileRect(index);
  if (confirm_ == index) {
    if (actionRect(tile, HitConfirmDelete).contains(position)) return HitConfirmDelete;
    if (actionRect(tile, HitConfirmCancel).contains(position)) return HitConfirmCancel;
    return HitNone;
  }
  for (Hit hit : {HitFavorite, HitCopy, HitDelete})
    if (actionRect(tile, hit).contains(position)) return hit;
  return HitNone;
}
void GalleryView::paintExtras(QPainter &painter, const QRect &rect, int index) {
  const MediaItem *item = model_->item(index);
  if (!item || columns_ > 10) return;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  if (confirm_ == index) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 166));
    painter.drawRoundedRect(rect, tileRadius(), tileRadius());
    const bool wide = columns_ <= 6;
    if (wide) {
      painter.setFont(Theme::font(12, QFont::Medium));
      painter.setPen(Qt::white);
      painter.drawText(QRect(rect.left() + 4, rect.center().y() - 26, rect.width() - 8, 20), Qt::AlignCenter, t("media.deleteConfirm"));
    }
    const QRect del = actionRect(rect, HitConfirmDelete), cancel = actionRect(rect, HitConfirmCancel);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xfb, 0x2c, 0x36, 204));
    painter.drawRoundedRect(del, Theme::kRadiusMd, Theme::kRadiusMd);
    painter.setBrush(QColor(255, 255, 255, 26));
    painter.drawRoundedRect(cancel, Theme::kRadiusMd, Theme::kRadiusMd);
    painter.setPen(Qt::white);
    if (wide) {
      Theme::drawIcon(painter, "trash", QRectF(del.left() + 8, del.center().y() - 7, 14, 14), Qt::white);
      painter.setFont(Theme::font(12, QFont::DemiBold));
      painter.drawText(del.adjusted(26, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, t("contextMenu.delete"));
      painter.setFont(Theme::font(12, QFont::Medium));
      painter.drawText(cancel, Qt::AlignCenter, t("common.cancel"));
    } else {
      Theme::drawIcon(painter, "trash", QRectF(del.center().x() - 7, del.center().y() - 7, 14, 14), Qt::white);
      Theme::drawIcon(painter, "x", QRectF(cancel.center().x() - 7, cancel.center().y() - 7, 14, 14), Qt::white);
    }
    painter.restore();
    return;
  }
  const auto grow = [&](Hit hit, const QRect &box) {
    if (hit != hoverHit_ || qFuzzyCompare(hitSpring_.value, 1)) return;
    const QPointF center = QRectF(box).center();
    painter.translate(center);
    painter.scale(hitSpring_.value, hitSpring_.value);
    painter.translate(-center);
  };
  painter.setOpacity(hoverAlpha_);
  const QRect heart = actionRect(rect, HitFavorite);
  painter.save();
  grow(HitFavorite, heart);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(0, 0, 0, 115));
  painter.drawRoundedRect(heart, Theme::kRadiusMd, Theme::kRadiusMd);
  painter.setOpacity(1);
  Theme::drawIcon(painter, item->favorite ? "heart-filled" : "heart", QRectF(heart.center().x() - 6.5, heart.center().y() - 6.5, 14, 14), item->favorite ? Theme::red500 : QColor(255, 255, 255, int(255 * hoverAlpha_)));
  painter.restore();
  painter.setOpacity(hoverAlpha_);
  // Shown up to 10 columns while hovered, above the picture.
  if (columns_ <= 10) paintDateBadge(painter, rect, *item);
  for (const auto &[hit, icon] : {std::pair<Hit, const char *>{HitCopy, "copy"}, {HitDelete, "trash"}}) {
    const QRect box = actionRect(rect, hit);
    painter.save();
    grow(hit, box);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 140));
    painter.drawRoundedRect(box, Theme::kRadiusLg, Theme::kRadiusLg);
    Theme::drawIcon(painter, icon, QRectF(box.center().x() - 7.5, box.center().y() - 7.5, 16, 16), QColor(255, 255, 255, 230));
    painter.restore();
  }
  painter.restore();
}
void GalleryView::paintGroups(QPainter &painter) {
  const int top = verticalScrollBar()->value();
  painter.setFont(Theme::font(14, QFont::DemiBold));
  for (const Group &group : groups_) {
    if (group.end < top || group.y > top + viewport()->height()) continue;
    // The header of the visible group stays pinned until the next group pushes it away.
    const qreal y = qMin(qMax<qreal>(group.y - top, 0), group.end - top - 44);
    painter.setPen(Theme::text);
    painter.drawText(QRectF(4, y, layoutWidth() - 8, 40), Qt::AlignVCenter | Qt::AlignLeft, group.name);
  }
}
void GalleryView::paintEmpty(QPainter &painter) {
  if (emptyTitle_.isEmpty()) return;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  QRect frame(2, 4, layoutWidth() - 14, viewport()->height());
  if (emptyFrame_) {
    frame.setHeight(qMax(320, window()->height() - 96));
    QPen dashed(QColor(255, 255, 255, 20), 2, Qt::CustomDashLine);
    dashed.setDashPattern({3, 3});
    painter.setPen(dashed);
    painter.setBrush(dropActive_ ? QColor(255, 255, 255, 8) : QColor(Qt::transparent));
    painter.drawRoundedRect(QRectF(frame).adjusted(1, 1, -1, -1), Theme::kRadius3xl, Theme::kRadius3xl);
  }
  const int cy = frame.center().y() - 8;
  const QRect iconBox(frame.center().x() - 28, cy - 62, 56, 56);
  painter.setPen(QPen(QColor(255, 255, 255, 13), 1));
  painter.setBrush(QColor(255, 255, 255, 13));
  painter.drawRoundedRect(QRectF(iconBox).adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRadius2xl, Theme::kRadius2xl);
  Theme::drawIcon(painter, emptyIcon_, QRectF(iconBox.center().x() - 13.5, iconBox.center().y() - 13.5, 28, 28), Theme::muted);
  painter.setPen(Theme::text);
  painter.setFont(Theme::font(16, QFont::Medium));
  painter.drawText(QRect(frame.x(), cy + 6, frame.width(), 24), Qt::AlignCenter, emptyTitle_);
  painter.setPen(Theme::muted);
  painter.setFont(Theme::font(14));
  painter.drawText(QRect(frame.x() + 40, cy + 32, frame.width() - 80, 44), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, emptySubtitle_);
  painter.restore();
}
void GalleryView::setEmptyHint(const QString &title, const QString &subtitle, const QByteArray &icon) {
  emptyTitle_ = title;
  emptySubtitle_ = subtitle;
  emptyIcon_ = icon;
  emptyFrame_ = icon == "upload";
  viewport()->update();
}
void GalleryView::paintEvent(QPaintEvent *event) {
  QPainter painter(viewport());
  const bool partial = event->rect() != viewport()->rect();
  if (partial) painter.setClipRegion(event->region());
  if (topFade_.isNull()) rebuildChrome();
  const auto [first, last] = visibleRange();
  // Opaque tiles replace the page, so it is drawn only into the gaps between them.
  // Shared copies, not pointers: a blur preview decoded for a later tile can rehash the cache the earlier ones live in.
  QVarLengthArray<QImage, 128> pictures;
  QRegion gaps = partial ? event->region() : QRegion(viewport()->rect());
  for (int index = first; index <= last; index++) {
    const QImage *picture = pictureFor(index);
    pictures.append(picture ? *picture : QImage());
    if (picture && !picture->hasAlphaChannel() && tileScale(index) >= 1) gaps -= tileRect(index);
  }
  const QPoint offset = viewport()->mapTo(window(), QPoint(0, 0));
  for (const QRect &gap : gaps) Theme::paintPage(painter, gap, offset);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  if (model_->total() == 0) paintEmpty(painter);
  const auto scaled = [&](int index, auto paint) {
    const qreal k = tileScale(index);
    if (qFuzzyCompare(k, 1)) return paint();
    const QPointF center = QRectF(tileRect(index)).center();
    painter.save();
    painter.translate(center);
    painter.scale(k, k);
    painter.translate(-center);
    paint();
    painter.restore();
  };
  // Tiles in motion go on top of their neighbours, the hovered one last.
  QVarLengthArray<int, 8> moving;
  for (int index = first; index <= last; index++) {
    if (springs_.contains(index)) {
      if (index != hovered_) moving.append(index);
      continue;
    }
    const QRect rect = tileRect(index);
    if (partial && !event->region().intersects(rect)) continue;
    const QImage &picture = pictures[index - first];
    paintTile(painter, rect, index, picture.isNull() ? nullptr : &picture);
  }
  if (springs_.contains(hovered_) && hovered_ >= first && hovered_ <= last) moving.append(hovered_);
  for (const int index : moving) {
    const QImage &picture = pictures[index - first];
    scaled(index, [&] { paintTile(painter, tileRect(index), index, picture.isNull() ? nullptr : &picture); });
  }
  if (hovered_ >= first && hovered_ <= last && !scrolling_) scaled(hovered_, [&] { paintExtras(painter, tileRect(hovered_), hovered_); });
  if (!groups_.isEmpty()) paintGroups(painter);
  // Fades over the content while there is more to scroll.
  QScrollBar *bar = verticalScrollBar();
  if (bar->value() > 0) painter.drawPixmap(0, 0, topFade_);
  if (bar->value() < bar->maximum()) painter.drawPixmap(0, viewport()->height() - 48, bottomFade_);
  if (dropActive_ && model_->total() > 0) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(255, 255, 255, 110), 2, Qt::DashLine));
    painter.setBrush(QColor(255, 255, 255, 12));
    painter.drawRoundedRect(QRectF(0, 0, layoutWidth(), viewport()->height()).adjusted(3, 3, -3, -3), Theme::kRadiusXl, Theme::kRadiusXl);
  }
  paintScrollIndicator(painter);
  noteFrame();
}
void GalleryView::paintScrollIndicator(QPainter &painter) {
  if (indicatorAlpha_ <= 0) return;
  paintScrollThumb(painter, scrollThumbRect(viewport()->width(), viewport()->height(), verticalScrollBar()->value(), verticalScrollBar()->maximum()), indicatorAlpha_);
}
void GalleryView::noteFrame() {
  if (!recording_) return;
  rec_.times.push_back(recordClock_.nsecsElapsed() / 1e6);
  rec_.ys.push_back(verticalScrollBar()->value());
  if (pendingWheel_ >= 0) {
    rec_.wheel.push_back(uptimeMs() - pendingWheel_);
    pendingWheel_ = -1;
  }
}
void GalleryView::updateDemand() {
  if (!isVisible()) return;  // The duplicates view owns the demand while it is on top.
  if (model_->total() <= 0) {
    pipeline_->setDemand({});
    return;
  }
  // Bounded prefetch: visible rows, then 10 rows on each side.
  const int total = model_->total();
  std::vector<ThumbRequest> demand;
  const auto add = [&](int index) {
    if (const MediaItem *item = model_->item(index)) {
      const QSize px = deviceSizeFor(index);
      if (px.width() == px.height()) demand.push_back({cacheKey(*item, px), item->thumbPath(), px.width(), 0});
      else demand.push_back({cacheKey(*item, px), item->thumbPath(), px.width(), px.height()});
    }
  };
  if (nonUniform()) {
    const auto [first, last] = visibleRange();
    const auto [farFirst, farLast] = visibleRange(viewport()->height());
    model_->require(qMax(0, farFirst), qMin(total - 1, farLast));
    demand.reserve(farLast - farFirst + 1);
    for (int index = first; index <= last; index++) add(index);
    for (int distance = 1; last + distance <= farLast || first - distance >= farFirst; distance++) {
      if (last + distance <= farLast) add(last + distance);
      if (first - distance >= farFirst) add(first - distance);
    }
    pipeline_->setDemand(std::move(demand));
    return;
  }
  const double s = step();
  const int top = verticalScrollBar()->value(), height = viewport()->height();
  const int firstRow = int(top / s), lastRow = int((top + height) / s);
  const int windowFirst = qMax(0, firstRow - 10) * columns_, windowLast = qMin(total - 1, (lastRow + 11) * columns_ - 1);
  model_->require(windowFirst, windowLast);
  demand.reserve(windowLast - windowFirst + 1);
  for (int index = firstRow * columns_; index <= qMin(total - 1, (lastRow + 1) * columns_ - 1); index++) add(index);
  for (int distance = 1; distance <= 11; distance++) {
    for (int c = 0; c < columns_; c++) {
      const int below = (lastRow + distance) * columns_ + c, above = (firstRow - distance) * columns_ + c;
      if (below <= windowLast) add(below);
      if (above >= windowFirst) add(above);
    }
  }
  pipeline_->setDemand(std::move(demand));
}
void GalleryView::scrollBy(qreal delta) {
  QScrollBar *bar = verticalScrollBar();
  const int duration = Theme::motion(160);
  if (!duration || recording_) {
    bar->setValue(bar->value() + qRound(delta));
    return;
  }
  wheelTarget_ = qBound<qreal>(0, (wheelTarget_ >= 0 ? wheelTarget_ : bar->value()) + delta, bar->maximum());
  smoothScroll_.stop();
  smoothScroll_.setStartValue(qreal(bar->value()));
  smoothScroll_.setEndValue(wheelTarget_);
  smoothScroll_.start();
}
void GalleryView::wheelEvent(QWheelEvent *event) {
  if (recording_ && pendingWheel_ < 0) pendingWheel_ = double(event->timestamp());
  if (confirm_ >= 0) {
    confirm_ = -1;
    viewport()->update();
  }
  event->accept();
  // macOS interleaves a trackpad gesture with empty events; one of those must not start the wheel glide, which would
  // pull the list back to its last target against the gesture.
  if (event->pixelDelta().y() == 0 && event->angleDelta().y() == 0) return;
  if (!event->pixelDelta().isNull() || event->phase() != Qt::NoScrollPhase) {
    // Trackpads deliver their own physics; a discrete wheel glides.
    smoothScroll_.stop();
    wheelTarget_ = -1;
    verticalScrollBar()->setValue(verticalScrollBar()->value() - event->pixelDelta().y());
  } else {
    scrollBy(-event->angleDelta().y() * (recording_ ? 0.5 : 0.8));
  }
}
int GalleryView::selectionCount() const { return all_ ? model_->total() - excluded_.size() : selected_.size(); }
bool GalleryView::isSelected(int index) const { return all_ ? !excluded_.contains(index) : selected_.contains(index); }
void GalleryView::clearSelection() {
  all_ = false;
  selected_.clear();
  excluded_.clear();
  viewport()->update();
  emit selectionChanged();
}
void GalleryView::selectAll() {
  all_ = true;
  selected_.clear();
  excluded_.clear();
  viewport()->update();
  emit selectionChanged();
}
void GalleryView::collectSelection(std::function<void(QVector<MediaItem>)> done) {
  const bool all = all_;
  const QSet<int> selected = selected_, excluded = excluded_;
  QVector<MediaItem> direct;
  // Everything selected is already in memory for ordinary selections; large ones read the full projection.
  bool complete = !all;
  if (complete) {
    direct.reserve(selected.size());
    for (int index : selected) {
      const MediaItem *item = model_->item(index);
      if (!item) {
        complete = false;
        break;
      }
      direct.push_back(*item);
    }
  }
  if (complete) return done(direct);
  model_->ensureGeometry([this, all, selected, excluded, done = std::move(done)] {
    QVector<MediaItem> chosen;
    const int total = model_->geometry().size();
    chosen.reserve(all ? total - excluded.size() : selected.size());
    for (int i = 0; i < total; i++)
      if (all ? !excluded.contains(i) : selected.contains(i)) chosen.push_back(model_->itemFromGeometry(i));
    done(chosen);
  });
}
void GalleryView::toggleIndex(int index) {
  if (all_) {
    excluded_.contains(index) ? (void)excluded_.remove(index) : (void)excluded_.insert(index);
  } else if (!selected_.remove(index)) {
    selected_.insert(index);
  }
  anchor_ = index;
  viewport()->update();
  emit selectionChanged();
}
void GalleryView::selectRangeTo(int index) {
  const int from = anchor_ >= 0 ? anchor_ : index;
  for (int i = qMin(from, index); i <= qMax(from, index); i++) {
    if (all_) excluded_.remove(i);
    else selected_.insert(i);
  }
  viewport()->update();
  emit selectionChanged();
}
void GalleryView::setBlocked(bool blocked) {
  blocked_ = blocked;
  hovered_ = confirm_ = -1;
  viewport()->update();
}
void GalleryView::mousePressEvent(QMouseEvent *event) {
  if (blocked_) return;
  if (event->button() != Qt::LeftButton) return;
  setFocus();
  const QPoint pos = event->pos();
  if (pos.x() >= layoutWidth() && verticalScrollBar()->maximum() > 0) {
    draggingThumb_ = true;
    verticalScrollBar()->setValue(int(qint64(pos.y()) * verticalScrollBar()->maximum() / qMax(1, viewport()->height())));
    return;
  }
  pressIndex_ = indexAt(pos);
  pressPos_ = pos;
  pressHandled_ = false;
  if (pressIndex_ < 0) {
    if (confirm_ >= 0) { confirm_ = -1; viewport()->update(); }
    return;
  }
  const Hit hit = hitAt(pressIndex_, pos);
  if (hit != HitNone) {
    hitSpring_.target = 0.9;
    if (!springTick_.isActive()) {
      springLast_ = springClock_.elapsed();
      springTick_.start();
    }
    pressHandled_ = true;
    const int index = pressIndex_;
    pressIndex_ = -1;
    switch (hit) {
      case HitFavorite: emit favoriteRequested(index); break;
      case HitCopy: emit copyRequested(index); break;
      case HitDelete: confirm_ = index; viewport()->update(tileRect(index)); break;
      case HitConfirmDelete: confirm_ = -1; emit deleteRequested(index); break;
      case HitConfirmCancel: confirm_ = -1; viewport()->update(tileRect(index)); break;
      default: break;
    }
    return;
  }
  if (confirm_ >= 0 && confirm_ != pressIndex_) confirm_ = -1;
  setTileTarget(pressIndex_, 0.98);
  const auto mods = event->modifiers();
  if (mods & Qt::ControlModifier) {  // Qt maps Cmd to Control on macOS.
    toggleIndex(pressIndex_);
    pressHandled_ = true;
  } else if (mods & Qt::ShiftModifier) {
    selectRangeTo(pressIndex_);
    pressHandled_ = true;
  }
}
void GalleryView::setHovered(int index, QPoint) {
  if (index == hovered_) return;
  const int previous = hovered_;
  hovered_ = index;
  setTileTarget(previous, 1);
  setTileTarget(index, index == pressIndex_ ? 0.98 : 1.027);
  setHoverHit(HitNone);
  if (previous >= 0) viewport()->update(tileRect(previous));
  if (index >= 0) {
    hoverAlpha_ = 0;
    hoverFade_.stop();
    if (Theme::reduceMotion()) {
      hoverAlpha_ = 1;
      viewport()->update(tileRect(index));
    } else {
      hoverFade_.start();
    }
  }
  if (confirm_ >= 0 && confirm_ != index) {
    viewport()->update(tileRect(confirm_));
    confirm_ = -1;
  }
}
void GalleryView::mouseMoveEvent(QMouseEvent *event) {
  if (blocked_) return;
  if (draggingThumb_) {
    verticalScrollBar()->setValue(int(qint64(event->pos().y()) * verticalScrollBar()->maximum() / qMax(1, viewport()->height())));
    return;
  }
  if (!(event->buttons() & Qt::LeftButton)) {
    if (!scrolling_) {
      const int index = indexAt(event->pos());
      setHovered(index, event->pos());
      const Hit hit = hitAt(index, event->pos());
      setHoverHit(hit);
      viewport()->setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    return;
  }
  if (pressIndex_ < 0) return;
  if ((event->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) return;
  const int index = pressIndex_;
  pressIndex_ = -1;
  setTileTarget(index, 1);
  startDrag(index);
}
void GalleryView::mouseReleaseEvent(QMouseEvent *event) {
  draggingThumb_ = false;
  if (pressIndex_ >= 0) setTileTarget(pressIndex_, pressIndex_ == hovered_ ? 1.027 : 1);
  if (hoverHit_ != HitNone) hitSpring_.target = 1.1;
  if (blocked_) return;
  if (event->button() == Qt::LeftButton && pressIndex_ >= 0 && !pressHandled_ && pressIndex_ == indexAt(event->pos())) {
    focus_ = pressIndex_;
    emit openRequested(pressIndex_);
  }
  pressIndex_ = -1;
}
void GalleryView::enterEvent(QEnterEvent *event) {
  QAbstractScrollArea::enterEvent(event);
  indicatorHide_.stop();
  indicatorFade_.stop();
  indicatorFade_.setStartValue(indicatorAlpha_);
  indicatorFade_.setEndValue(1.0);
  indicatorFade_.start();
}
void GalleryView::leaveEvent(QEvent *event) {
  QAbstractScrollArea::leaveEvent(event);
  setHovered(-1, {});
  indicatorHide_.start(600);
}
void GalleryView::focusInEvent(QFocusEvent *event) { QAbstractScrollArea::focusInEvent(event); if (focus_ >= 0) viewport()->update(tileRect(focus_)); }
void GalleryView::focusOutEvent(QFocusEvent *event) { QAbstractScrollArea::focusOutEvent(event); if (focus_ >= 0) viewport()->update(tileRect(focus_)); }
bool GalleryView::viewportEvent(QEvent *event) { return QAbstractScrollArea::viewportEvent(event); }
// The visible picked tiles shrink toward the pointer for 400ms while the drag starts.
class DragFlyers : public QWidget {
public:
  struct Flyer {
    QRectF rect;
    QImage image;
    QPointF center;
  };
  DragFlyers(QWidget *host, QVector<Flyer> flyers) : QWidget(host), flyers_(std::move(flyers)) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setGeometry(host->rect());
    show();
    raise();
    connect(&tick_, &QTimer::timeout, this, [this] {
      const QPointF target = mapFromGlobal(QCursor::pos());
      for (Flyer &flyer : flyers_) flyer.center += (target - flyer.center) * 0.15;
      scale_ -= (scale_ - 0.2) * 0.15;
      update();
    });
    tick_.start(16);
    QTimer::singleShot(400, this, &QObject::deleteLater);
  }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const Flyer &flyer : flyers_) {
      QRectF box(QPointF(0, 0), flyer.rect.size() * scale_);
      box.moveCenter(flyer.center);
      const qreal radius = Theme::kRadius2xl * scale_;
      QPainterPath clip;
      clip.addRoundedRect(box, radius, radius);
      p.save();
      p.setClipPath(clip);
      if (flyer.image.isNull()) p.fillRect(box, Theme::secondary);
      else p.drawImage(box, flyer.image);
      p.restore();
    }
  }
private:
  QVector<Flyer> flyers_;
  QTimer tick_;
  qreal scale_ = 1;
};
void GalleryView::startDrag(int index) {
  if (!isSelected(index) && selectionCount() > 0) {
    // A drag of an unselected tile while something is selected adds nothing.
    return;
  }
  const bool transient = !isSelected(index);
  if (transient) {
    selected_.insert(index);
    viewport()->update();
    emit selectionChanged();
  }
  auto *drag = new QDrag(this);
  auto *mime = new QMimeData;
  const int count = selectionCount();
  mime->setData(QString::fromLatin1(kInternalMime), QByteArray::number(count));
  QList<QUrl> urls;
  QStringList stackKeys;
  const auto addUrl = [&](const MediaItem &item) { urls.push_back(QUrl::fromLocalFile(item.path())); };
  if (all_ || count > 2000) {
    if (model_->geometryReady()) {
      for (int i = 0; i < model_->geometry().size(); i++)
        if (isSelected(i)) addUrl(model_->itemFromGeometry(i));
    } else {
      model_->ensureGeometry();  // Internal moves still work; the next drag can also leave the app.
    }
  } else {
    for (int i : selected_) {
      if (const MediaItem *item = model_->item(i)) addUrl(*item);
      else if (i < model_->geometry().size() && model_->geometryReady()) addUrl(model_->itemFromGeometry(i));
    }
  }
  if (!urls.isEmpty()) mime->setUrls(urls);
  drag->setMimeData(mime);
  // Up to three 80px thumbnails offset by 8px, with the total over them past three.
  const qreal ratio = devicePixelRatioF();
  QPixmap preview(QSize(128, 128) * ratio);
  preview.fill(Qt::transparent);
  preview.setDevicePixelRatio(ratio);
  {
    QPainter p(&preview);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QVector<int> indices;
    indices.push_back(index);
    if (!all_) {
      for (int i : selected_) {
        if (i != index) indices.push_back(i);
        if (indices.size() == 3) break;
      }
    } else {
      for (int i = 0; i < model_->total() && indices.size() < 3; i++)
        if (i != index && isSelected(i)) indices.push_back(i);
    }
    for (int layer = 0; layer < indices.size(); layer++) {
      const QRectF box(8 * layer + 16, 8 * layer + 16, 80, 80);
      QPainterPath clip;
      clip.addRoundedRect(box, Theme::kRadiusMd, Theme::kRadiusMd);
      p.save();
      p.setClipPath(clip);
      p.fillRect(box, Theme::secondary);
      if (const MediaItem *item = model_->item(indices[layer])) {
        const QImage *image = pipeline_->get(cacheKey(*item, deviceSizeFor(indices[layer])));
        if (!image) image = model_->blur(*item);
        if (image) p.drawImage(box, *image);
      }
      p.restore();
      p.setPen(QPen(QColor(0, 0, 0, 64), 1));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(box, Theme::kRadiusMd, Theme::kRadiusMd);
    }
    if (count > 3) {
      const QRectF box(32, 32, 80, 80);
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(0, 0, 0, 102));
      p.drawRoundedRect(box, Theme::kRadiusMd, Theme::kRadiusMd);
      p.setPen(Qt::white);
      p.setFont(Theme::font(20, QFont::Bold));
      p.drawText(box, Qt::AlignCenter, QString::number(count));
    }
  }
  drag->setPixmap(preview);
  drag->setHotSpot(QPoint(64, 64));
  if (QWidget *host = window()->findChild<QWidget *>(QStringLiteral("overlayHost")); host && Theme::motion(1)) {
    QVector<DragFlyers::Flyer> flyers;
    const auto [first, last] = visibleRange();
    for (int i = first; i <= last; i++) {
      const MediaItem *item = model_->item(i);
      if (!item || !isSelected(i)) continue;
      const QRect tile = tileRect(i);
      const QRectF inHost(viewport()->mapTo(host, tile.topLeft()), QSizeF(tile.size()));
      const QImage *image = pipeline_->get(cacheKey(*item, deviceSizeFor(i)));
      if (!image) image = model_->blur(*item);
      flyers.push_back({inHost, image ? *image : QImage(), inHost.center()});
    }
    if (!flyers.isEmpty()) new DragFlyers(host, std::move(flyers));
  }
  const Qt::DropAction action = drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
  Q_UNUSED(action);
  emit dragFinished();
  if (transient && selected_.size() == 1 && selected_.contains(index)) {
    selected_.clear();
    viewport()->update();
    emit selectionChanged();
  }
}
void GalleryView::focusIndex(int index) {
  focus_ = qBound(0, index, qMax(0, model_->total() - 1));
  ensureVisible(focus_);
  viewport()->update();
}
void GalleryView::ensureVisible(int index) {
  const QRect r = tileRect(index);
  // Clear of the 48-point edge fades when the viewport has room for that.
  const int margin = qBound(0, (viewport()->height() - r.height()) / 2, 48);
  if (r.top() < margin) verticalScrollBar()->setValue(verticalScrollBar()->value() + r.top() - margin);
  else if (r.bottom() > viewport()->height() - margin) verticalScrollBar()->setValue(verticalScrollBar()->value() + r.bottom() - viewport()->height() + margin);
}
void GalleryView::keyPressEvent(QKeyEvent *event) {
  if (blocked_) return;
  const int total = model_->total();
  const auto noMods = !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
  if (event->key() == Qt::Key_Escape) {
    QAbstractScrollArea::keyPressEvent(event);  // Escape does not clear the gallery selection.
  } else if ((event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && noMods) {
    if (focus_ < 0 && total > 0) focus_ = visibleRange().first;
    if (focus_ >= 0) emit openRequested(focus_);
  } else if (total > 0 && noMods && (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
    focusIndex(event->key() == Qt::Key_Home ? 0 : total - 1);
  } else if (total > 0 && noMods && (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
    const int base = focus_ < 0 ? visibleRange().first : focus_;
    int next = base;
    if (event->key() == Qt::Key_Right) next = base + 1;
    else if (event->key() == Qt::Key_Left) next = base - 1;
    else if (!nonUniform()) next = base + (event->key() == Qt::Key_Down ? columns_ : -columns_);
    else if (base < rects_.size()) {
      // Nearest tile straight above or below in free-form layouts.
      const QRectF from = rects_[base];
      const bool down = event->key() == Qt::Key_Down;
      qreal best = 1e12;
      for (int i = qMax(0, base - columns_ * 4); i <= qMin(int(rects_.size()) - 1, base + columns_ * 4); i++) {
        if (i == base) continue;
        const QRectF &r = rects_[i];
        if (down ? r.top() <= from.top() : r.top() >= from.top()) continue;
        const qreal score = qAbs(r.center().x() - from.center().x()) * 4 + qAbs(r.center().y() - from.center().y());
        if (score < best) { best = score; next = i; }
      }
    }
    if (event->modifiers() & Qt::ShiftModifier) {
      if (anchor_ < 0) anchor_ = base;
      const int clamped = qBound(0, next, total - 1);
      selectRangeTo(clamped);
    }
    focusIndex(next);
  } else {
    QAbstractScrollArea::keyPressEvent(event);
  }
}
void GalleryView::contextMenuEvent(QContextMenuEvent *event) {
  if (blocked_) return;
  const int index = indexAt(event->pos());
  if (index >= 0) emit contextRequested(index, event->globalPos());
}
void GalleryView::dragEnterEvent(QDragEnterEvent *event) {
  if (!blocked_ && event->mimeData()->hasUrls() && !event->mimeData()->hasFormat(QString::fromLatin1(kInternalMime))) {
    event->acceptProposedAction();
    dropActive_ = true;
    viewport()->update();
  }
}
void GalleryView::dragMoveEvent(QDragMoveEvent *event) {
  if (!blocked_ && event->mimeData()->hasUrls() && !event->mimeData()->hasFormat(QString::fromLatin1(kInternalMime))) event->acceptProposedAction();
}
void GalleryView::dragLeaveEvent(QDragLeaveEvent *) {
  dropActive_ = false;
  viewport()->update();
}
void GalleryView::dropEvent(QDropEvent *event) {
  dropActive_ = false;
  viewport()->update();
  QStringList paths;
  for (const QUrl &url : event->mimeData()->urls())
    if (url.isLocalFile()) paths << url.toLocalFile();
  if (!paths.isEmpty() && !event->mimeData()->hasFormat(QString::fromLatin1(kInternalMime))) {
    event->acceptProposedAction();
    emit filesDropped(paths);
  }
}
GalleryView::Visible GalleryView::visibleState(QVector<QPair<QString, bool>> *keys) {
  Visible v;
  const int total = model_->total();
  int first, last;
  if (nonUniform()) {
    if (!model_->geometryReady() && total > 0) {
      v.expected = v.blank = 1;
      return v;
    }
    int f, l;
    std::tie(f, l) = visibleRange();
    first = f;
    last = l + 1;
    // Fully above the viewport does not count as visible.
    const int top = verticalScrollBar()->value();
    while (first < last && rects_[first].bottom() <= top) first++;
  } else {
    const double s = step();
    const int top = verticalScrollBar()->value(), height = viewport()->height();
    const int firstRow = int((top + gap()) / s), lastRow = int(std::ceil((top + height) / s));
    first = qMin(total, firstRow * columns_);
    last = qMin(total, lastRow * columns_);
  }
  v.expected = last - first;
  for (int index = first; index < last; index++) {
    const MediaItem *item = model_->item(index);
    if (!item) {
      v.blank++;
      continue;
    }
    v.visible++;
    const bool ok = pipeline_->has(cacheKey(*item, deviceSizeFor(index)));
    if (ok) v.ready++;
    if (keys) keys->push_back({item->path(), ok});
  }
  return v;
}
void GalleryView::startRecording() {
  rec_ = {};
  rec_.times.reserve(30000);
  rec_.ys.reserve(30000);
  recordClock_.start();
  pendingWheel_ = -1;
  recording_ = true;
  sampler_.start();
}
FrameRecording GalleryView::stopRecording() {
  recording_ = false;
  sampler_.stop();
  return std::move(rec_);
}
VirtualListSource GalleryView::accessibleSource() {
  VirtualListSource source;
  // Only tiles on screen are exposed to accessibility: listing every photo of a large album is too expensive.
  source.count = [this] {
    const auto [first, last] = visibleRange();
    return qMax(0, last - first + 1);
  };
  source.name = [this](int i) {
    const MediaItem *item = model_->item(visibleRange().first + i);
    return item ? item->name : QString();
  };
  source.description = [this](int i) {
    const MediaItem *item = model_->item(visibleRange().first + i);
    return item ? formatMediaDate(item->displayDate(), false) : QString();
  };
  source.rect = [this](int i) {
    const QRect r = tileRect(visibleRange().first + i).intersected(viewport()->rect());
    return r.isEmpty() ? QRect() : QRect(viewport()->mapToGlobal(r.topLeft()), r.size());
  };
  source.selected = [this](int i) { return isSelected(visibleRange().first + i); };
  source.activate = [this](int i) { emit openRequested(visibleRange().first + i); };
  return source;
}
