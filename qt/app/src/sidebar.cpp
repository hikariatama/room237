#include "sidebar.h"
#include "i18n.h"
#include "thumbs.h"
#include <QApplication>
#include <QFontMetricsF>
#include <QDrag>
#include <QFileInfo>
#include <QImageReader>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QLinearGradient>
#include <QPainterPath>
#include <QScrollBar>
#include <QUrl>
static const char *kAlbumMime = "application/x-room237-album";
static const char *kMediaMime = "application/x-room237";
static constexpr int kRow = 44, kRowHeight = 40, kRightPad = 0;
AlbumTree::AlbumTree(QWidget *parent) : QAbstractScrollArea(parent) {
  setFrameShape(QFrame::NoFrame);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  viewport()->setMouseTracking(true);
  viewport()->setAcceptDrops(true);
  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);
  viewport()->setAutoFillBackground(false);
  setStyleSheet(QStringLiteral("background:transparent"));
  hoverExpand_.setSingleShot(true);
  hoverExpand_.setInterval(600);
  connect(&hoverExpand_, &QTimer::timeout, this, [this] {
    if (!hoverExpandTarget_.isEmpty()) setExpanded(hoverExpandTarget_, true);
  });
  iconPool_.setMaxThreadCount(1);
  iconPool_.setExpiryTimeout(2000);
  hoverAnim_.setDuration(100);
  hoverAnim_.setStartValue(0.0);
  hoverAnim_.setEndValue(1.0);
  connect(&hoverAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    hoverT_ = v.toReal();
    viewport()->update();
  });
  appear_.setDuration(150);
  appear_.setStartValue(0.0);
  appear_.setEndValue(1.0);
  connect(&appear_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    appearProgress_ = v.toReal();
    layoutRows();
    viewport()->update();
  });
  connect(&appear_, &QVariantAnimation::finished, this, [this] {
    appearingIds_.clear();
    appearProgress_ = 1;
    layoutRows();
    viewport()->update();
  });
}
AlbumTree::~AlbumTree() {
  iconPool_.clear();
  iconPool_.waitForDone();
}
void AlbumTree::setAlbums(const QVector<AlbumInfo> &albums, int favorites) {
  albums_ = albums;
  favorites_ = favorites;
  // An icon that could not load yet may exist now: the indexer produces them over time.
  for (auto it = icons_.begin(); it != icons_.end();) it = it->isNull() ? icons_.erase(it) : std::next(it);
  rebuildRows();
}
void AlbumTree::rebuildRows() {
  rows_.clear();
  if (favorites_ > 0) {
    Row row;
    row.id = kFavoritesId;
    row.name = t("favorites.album");
    row.count = favorites_;
    row.favorites = true;
    rows_.push_back(row);
  }
  QHash<QString, QVector<const AlbumInfo *>> children;
  QSet<QString> known;
  for (const AlbumInfo &album : albums_) known.insert(album.id);
  for (const AlbumInfo &album : albums_) children[known.contains(album.parent) ? album.parent : QString()].push_back(&album);
  // The count on a row is album.totalSize: the album and everything nested in it.
  QHash<QString, int> totals;
  for (const AlbumInfo &album : albums_) {
    for (QString id = album.id; !id.isEmpty(); id = id.contains(QLatin1Char('/')) ? id.left(id.lastIndexOf(QLatin1Char('/'))) : QString()) totals[id] += album.size;
  }
  const std::function<void(const QString &, int)> walk = [&](const QString &parent, int depth) {
    auto &level = children[parent];
    std::stable_sort(level.begin(), level.end(), [](const AlbumInfo *a, const AlbumInfo *b) { return a->name.compare(b->name, Qt::CaseInsensitive) < 0; });
    for (const AlbumInfo *album : level) {
      Row row;
      row.id = album->id;
      row.name = album->name;
      row.path = album->path;
      row.thumb = album->thumbPath;
      row.depth = depth;
      row.count = totals.value(album->id);
      row.hasChildren = children.contains(album->id) && !children[album->id].isEmpty();
      rows_.push_back(row);
      if (row.hasChildren && expanded_.contains(album->id)) walk(album->id, depth + 1);
    }
  };
  walk(QString(), 0);
  QSet<QString> ids;
  for (const Row &row : rows_) ids.insert(row.id);
  if (!knownIds_.isEmpty() && Theme::motion(150)) {
    appearingIds_.clear();
    for (const QString &id : ids)
      if (!knownIds_.contains(id)) appearingIds_.insert(id);
    if (!appearingIds_.isEmpty()) {
      appearProgress_ = 0;
      appear_.stop();
      appear_.start();
    }
  }
  knownIds_ = ids;
  layoutRows();
  updateScroll();
  viewport()->update();
  QAccessibleEvent event(this, QAccessible::ObjectReorder);
  QAccessible::updateAccessibility(&event);
}
qreal AlbumTree::appearing(int row) const {
  return row < rows_.size() && appearingIds_.contains(rows_[row].id) ? QEasingCurve(QEasingCurve::OutCubic).valueForProgress(appearProgress_) : 1;
}
void AlbumTree::layoutRows() {
  tops_.resize(rows_.size() + 1);
  qreal y = 0;
  for (int i = 0; i < rows_.size(); i++) {
    tops_[i] = qRound(y);
    y += kRow * appearing(i);
  }
  tops_[rows_.size()] = qRound(y);
}
// The full height, also while new rows are still growing in, so a scroll to them is not cut short.
int AlbumTree::contentMax() const { return qMax(0, int(rows_.size()) * kRow + kRow + 8 - viewport()->height()); }
void AlbumTree::updateScroll() {
  verticalScrollBar()->setPageStep(viewport()->height());
  verticalScrollBar()->setSingleStep(kRow);
  verticalScrollBar()->setRange(0, qMax(slackMax_, contentMax()));
}
void AlbumTree::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  updateScroll();
}
int AlbumTree::rowIndex(const QString &id) const {
  for (int i = 0; i < rows_.size(); i++)
    if (rows_[i].id == id) return i;
  return -1;
}
QRect AlbumTree::rowRect(int row) const {
  const int top = row >= 0 && row < tops_.size() ? tops_[row] : row * kRow;
  return {0, top - verticalScrollBar()->value(), viewport()->width() - kRightPad, kRowHeight};
}
QRect AlbumTree::newAlbumRect() const {
  QRect r = rowRect(rows_.size());
  r.setHeight(44);
  return r;
}
QRect AlbumTree::rectFor(const QString &albumId) const {
  const int row = rowIndex(albumId);
  return row < 0 ? QRect() : rowRect(row);
}
QRect AlbumTree::chevronRect(int row) const {
  const QRect r = rowRect(row);
  return {r.right() - 60, r.top() + 12, 20, 20};
}
int AlbumTree::rowAt(QPoint point) const {
  for (int row = 0; row < rows_.size(); row++)
    if (rowRect(row).contains(point)) return row;
  return -1;
}
void AlbumTree::setActive(const QString &id) {
  active_ = id;
  const int row = rowIndex(id);
  if (row >= 0) {
    focusRow_ = row;
    const QRect r = rowRect(row);
    if (r.top() < 0) verticalScrollBar()->setValue(verticalScrollBar()->value() + r.top());
    else if (r.bottom() > viewport()->height()) verticalScrollBar()->setValue(verticalScrollBar()->value() + r.bottom() - viewport()->height());
  }
  viewport()->update();
}
void AlbumTree::collapseAutoExpandedExcept(const QString &keep) {
  const int before = rowIndex(keep), value = verticalScrollBar()->value();
  QSet<QString> kept;
  for (QString id = keep; !id.isEmpty(); id = id.contains(QLatin1Char('/')) ? id.left(id.lastIndexOf(QLatin1Char('/'))) : QString()) kept.insert(id);
  bool changed = false;
  for (auto it = expanded_.begin(); it != expanded_.end();) {
    if (kept.contains(*it) || manuallyExpanded_.contains(*it)) {
      ++it;
    } else {
      it = expanded_.erase(it);
      changed = true;
    }
  }
  if (!changed) return;
  // The chosen row stays under the pointer: rows that fold away above it move the list with them, and rows that fold
  // away below it at the end of the list leave room at the bottom, which scrolling up takes back.
  slackMax_ = value;
  rebuildRows();
  const int after = rowIndex(keep);
  const int target = before >= 0 && after >= 0 ? value + (after - before) * kRow : value;
  slackMax_ = target > contentMax() ? target : -1;
  updateScroll();
  verticalScrollBar()->setValue(target);
}
void AlbumTree::expandTo(const QString &id, bool self) {
  QString current = id;
  bool changed = false;
  if (self && !id.isEmpty() && !expanded_.contains(id)) {
    expanded_.insert(id);
    changed = true;
  }
  while (true) {
    const qsizetype slash = current.lastIndexOf(QLatin1Char('/'));
    if (slash < 0) break;
    current = current.left(slash);
    if (!expanded_.contains(current)) {
      expanded_.insert(current);
      changed = true;
    }
  }
  if (changed) rebuildRows();
}
void AlbumTree::setExpanded(const QString &id, bool expanded) {
  if (expanded == expanded_.contains(id)) return;
  if (expanded) expanded_.insert(id);
  else expanded_.remove(id);
  rebuildRows();
}
void AlbumTree::setBlocked(bool blocked) {
  blocked_ = blocked;
  viewport()->update();
}
void AlbumTree::loadIcon(const QString &thumb) {
  if (thumb.isEmpty() || iconsLoading_.contains(thumb) || icons_.contains(thumb)) return;
  iconsLoading_.insert(thumb);
  const int side = qRound(28 * devicePixelRatioF());
  iconPool_.start([this, thumb, side] {
    QImage image = decodeThumbnail(thumb, side);
    QMetaObject::invokeMethod(this, [this, thumb, image = std::move(image)] {
      iconsLoading_.remove(thumb);
      icons_.insert(thumb, image);
      viewport()->update();
    }, Qt::QueuedConnection);
  });
}
void AlbumTree::paintEvent(QPaintEvent *) {
  QPainter p(viewport());
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  const int first = 0, last = int(rows_.size());
  const QFont nameFont = Theme::font(14);
  QFont countFont = Theme::font(14);
  countFont.setFeature("tnum", 1);
  for (int i = first; i < last && i < rows_.size(); i++) {
    const Row &row = rows_[i];
    const QRect r = rowRect(i);
    if (r.bottom() < 0 || r.top() > viewport()->height()) continue;
    const qreal grown = appearing(i);
    p.save();
    if (grown < 1) {
      p.setOpacity(grown);
      p.setClipRect(QRect(r.left(), r.top(), r.width(), qCeil(kRowHeight * grown)));
    }
    const bool active = row.id == active_ && !blocked_;
    const bool dropping = dropTarget_ == row.id;
    // 6.x: the open album bg-white/5, a hovered one bg-white/10, fading over 100 ms.
    const qreal hover = i == hovered_ ? hoverT_ : i == fadingOut_ ? 1 - hoverT_ : 0;
    if (active || dropping || hover > 0) {
      p.setPen(dropping ? QPen(QColor(255, 255, 255, 51), 2) : QPen(Qt::NoPen));
      p.setBrush(dropping ? QColor(255, 255, 255, 20) : active ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, qRound(26 * hover)));
      p.drawRoundedRect(QRectF(r).adjusted(dropping ? 1 : 0, dropping ? 1 : 0, dropping ? -1 : 0, dropping ? -1 : 0), Theme::kRadiusXl, Theme::kRadiusXl);
    }
    if (i == focusRow_ && hasFocus() && focusVisible()) {
      p.setPen(QPen(QColor(255, 255, 255, 89), 2));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), Theme::kRadiusXl - 1, Theme::kRadiusXl - 1);
    }
    const int indent = row.depth * 14;
    const QRectF icon(r.left() + 6 + indent, r.top() + 6, 28, 28);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 51));
    bool painted = false;
    if (!row.favorites && !blocked_ && !row.thumb.isEmpty()) {
      auto it = icons_.constFind(row.thumb);
      if (it == icons_.cend()) {
        const_cast<AlbumTree *>(this)->loadIcon(row.thumb);
      } else if (!it->isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(icon, Theme::kRadiusLg, Theme::kRadiusLg);
        p.save();
        p.setClipPath(clip);
        p.drawImage(icon, *it);
        p.restore();
        painted = true;
      }
    }
    if (!painted) p.drawRoundedRect(icon, Theme::kRadiusLg, Theme::kRadiusLg);
    if (row.favorites) Theme::drawIcon(p, "heart-filled", QRectF(icon.center().x() - 8, icon.center().y() - 8, 16, 16), Theme::red500);
    const QString count = QString::number(row.count);
    const int countWidth = QFontMetrics(countFont).horizontalAdvance(count);
    const int countRight = r.right() + 1 - 10;
    p.setFont(countFont);
    p.setPen(Theme::muted);
    p.drawText(QRect(countRight - countWidth, r.top(), countWidth, r.height()), Qt::AlignVCenter | Qt::AlignRight, count);
    const int textLeft = int(icon.right()) + 8;
    int textRight = countRight - countWidth - 8;
    if (row.hasChildren) {
      textRight -= 18;
      const QRectF chevron(countRight - countWidth - 8 - 16, r.top() + 14, 12, 12);
      p.save();
      p.translate(chevron.center());
      if (expanded_.contains(row.id)) p.rotate(90);
      Theme::drawIcon(p, "chevron-right", QRectF(-6, -6, 12, 12), Theme::muted);
      p.restore();
    }
    p.setFont(nameFont);
    p.setPen(Theme::text);
    p.drawText(QRect(textLeft, r.top(), textRight - textLeft, r.height()), Qt::AlignVCenter | Qt::AlignLeft, Theme::elided(nameFont, row.name, textRight - textLeft));
    p.restore();
  }
  // 6.x: a ghost button across the sidebar, a plus and the label centred in it.
  const QRect add = newAlbumRect();
  if (add.top() < viewport()->height() && !blocked_) {
    const QRectF button(add.left(), add.top(), add.width(), 36);
    if (hoveredNew_) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(229, 229, 229, 26));
      p.drawRoundedRect(button, 18, 18);
    }
    const QFont font = Theme::font(14, QFont::Medium);
    const QString label = t("album.new");
    const qreal width = 16 + 8 + QFontMetricsF(font).horizontalAdvance(label);
    const qreal left = button.center().x() - width / 2;
    Theme::drawIcon(p, "plus", QRectF(left, button.center().y() - 8, 16, 16), Theme::text);
    p.setPen(Theme::text);
    p.setFont(font);
    p.drawText(QRectF(left + 24, button.top(), width, button.height()), Qt::AlignVCenter | Qt::AlignLeft, label);
  }
  const int value = verticalScrollBar()->value();
  const QRect width(0, 0, viewport()->width() - kRightPad, 40);
  if (value > 0) {
    QLinearGradient top(0, 0, 0, 40);
    top.setColorAt(0, Theme::sidebar);
    top.setColorAt(1, QColor(0x15, 0x14, 0x14, 0));
    p.fillRect(width, top);
  }
  if (value < verticalScrollBar()->maximum()) {
    const QRect bottomRect = width.translated(0, viewport()->height() - 40);
    QLinearGradient bottom(0, bottomRect.bottom(), 0, bottomRect.top());
    bottom.setColorAt(0, Theme::sidebar);
    bottom.setColorAt(1, QColor(0x15, 0x14, 0x14, 0));
    p.fillRect(bottomRect, bottom);
  }
}
void AlbumTree::mousePressEvent(QMouseEvent *event) {
  if (blocked_ || event->button() != Qt::LeftButton) return;
  setFocus();
  pressed_ = rowAt(event->pos());
  if (pressed_ >= 0) focusRow_ = pressed_;
  pressPos_ = event->pos();
  if (newAlbumRect().contains(event->pos())) {
    pressed_ = -1;
    emit newAlbumRequested(newAlbumRect());
  }
}
void AlbumTree::mouseMoveEvent(QMouseEvent *event) {
  if (blocked_) return;
  if (event->buttons() & Qt::LeftButton) {
    if (pressed_ < 0 || rows_[pressed_].favorites || (event->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) return;
    const Row row = rows_[pressed_];
    pressed_ = -1;
    auto *drag = new QDrag(this);
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kAlbumMime), row.id.toUtf8());
    drag->setMimeData(mime);
    const qreal ratio = devicePixelRatioF();
    const int w = qMin(220, QFontMetrics(Theme::font(14)).horizontalAdvance(row.name) + 28);
    QPixmap pixmap(QSize(w, 32) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    {
      QPainter p(&pixmap);
      p.setRenderHint(QPainter::Antialiasing);
      p.setPen(QPen(Theme::border, 1));
      p.setBrush(QColor(0x15, 0x14, 0x14, 240));
      p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, 31), 10, 10);
      p.setPen(Theme::text);
      p.setFont(Theme::font(14));
      p.drawText(QRect(14, 0, w - 20, 32), Qt::AlignVCenter | Qt::AlignLeft, Theme::elided(p.font(), row.name, w - 20));
    }
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(w / 2, 16));
    drag->exec(Qt::MoveAction);
    return;
  }
  const int row = rowAt(event->pos());
  const bool newRow = newAlbumRect().contains(event->pos());
  if (row != hovered_ || newRow != hoveredNew_) {
    if (row != hovered_) fadeHover(row);
    hoveredNew_ = newRow;
    viewport()->setCursor(row >= 0 || newRow ? Qt::PointingHandCursor : Qt::ArrowCursor);
    viewport()->update();
  }
}
void AlbumTree::mouseReleaseEvent(QMouseEvent *event) {
  if (blocked_ || event->button() != Qt::LeftButton) return;
  const int row = rowAt(event->pos());
  if (row >= 0 && row == pressed_) {
    const Row &target = rows_[row];
    const QRect r = rowRect(row);
    const int countWidth = QFontMetrics(Theme::font(14)).horizontalAdvance(QString::number(target.count));
    const QRect chevron(r.right() + 1 - 10 - countWidth - 8 - 20, r.top() + 10, 20, 20);
    if (target.hasChildren && chevron.contains(event->pos())) {
      const bool next = !expanded_.contains(target.id);
      if (next) manuallyExpanded_.insert(target.id);
      else manuallyExpanded_.remove(target.id);
      setExpanded(target.id, next);
    } else {
      focusRow_ = row;
      emit activated(target.id);
    }
  }
  pressed_ = -1;
}
void AlbumTree::fadeHover(int row) {
  fadingOut_ = hovered_;
  hovered_ = row;
  hoverAnim_.stop();
  if (!Theme::motion(100)) {
    hoverT_ = 1;
    fadingOut_ = -1;
    viewport()->update();
    return;
  }
  hoverT_ = 0;
  hoverAnim_.start();
}
void AlbumTree::leaveEvent(QEvent *event) {
  QAbstractScrollArea::leaveEvent(event);
  fadeHover(-1);
  hoveredNew_ = false;
  viewport()->update();
}
void AlbumTree::wheelEvent(QWheelEvent *event) {
  QPoint delta = event->pixelDelta();
  if (delta.isNull()) delta = event->angleDelta() / 2;
  verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
  // Room left at the bottom by folded rows shrinks as the list scrolls up, never with a jump.
  if (slackMax_ >= 0) {
    slackMax_ = verticalScrollBar()->value() > contentMax() ? verticalScrollBar()->value() : -1;
    updateScroll();
  }
  event->accept();
}
void AlbumTree::keyPressEvent(QKeyEvent *event) {
  if (blocked_ || rows_.isEmpty()) return QAbstractScrollArea::keyPressEvent(event);
  if (focusRow_ < 0 || focusRow_ >= rows_.size()) focusRow_ = qMax(0, rowIndex(active_));
  const Row row = rows_[focusRow_];
  switch (event->key()) {
    case Qt::Key_Down: focusRow_ = qMin(int(rows_.size()) - 1, focusRow_ + 1); break;
    case Qt::Key_Up: focusRow_ = qMax(0, focusRow_ - 1); break;
    case Qt::Key_Home: focusRow_ = 0; break;
    case Qt::Key_End: focusRow_ = rows_.size() - 1; break;
    case Qt::Key_Right:
      if (row.hasChildren) {
        if (!expanded_.contains(row.id)) { manuallyExpanded_.insert(row.id); setExpanded(row.id, true); }
        else focusRow_++;
      }
      break;
    case Qt::Key_Left:
      if (row.hasChildren && expanded_.contains(row.id)) {
        manuallyExpanded_.remove(row.id);
        setExpanded(row.id, false);
      } else if (row.depth > 0) {
        const QString parent = row.id.left(row.id.lastIndexOf(QLatin1Char('/')));
        const int up = rowIndex(parent);
        if (up >= 0) focusRow_ = up;
      }
      break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
      emit activated(row.id);
      break;
    case Qt::Key_Menu:
      emit contextRequested(row.id, viewport()->mapToGlobal(rowRect(focusRow_).center()));
      break;
    default:
      return QAbstractScrollArea::keyPressEvent(event);
  }
  const QRect r = rowRect(focusRow_);
  if (r.top() < 0) verticalScrollBar()->setValue(verticalScrollBar()->value() + r.top());
  else if (r.bottom() > viewport()->height()) verticalScrollBar()->setValue(verticalScrollBar()->value() + r.bottom() - viewport()->height());
  viewport()->update();
}
void AlbumTree::contextMenuEvent(QContextMenuEvent *event) {
  if (blocked_) return;
  const int row = rowAt(event->pos());
  if (row >= 0 && !rows_[row].favorites) emit contextRequested(rows_[row].id, event->globalPos());
}
void AlbumTree::clearDrop() {
  dropTarget_.clear();
  hoverExpandTarget_.clear();
  hoverExpand_.stop();
  emit dragHint({}, {});
  viewport()->update();
}
void AlbumTree::dragEnterEvent(QDragEnterEvent *event) {
  if (blocked_) return;
  const QMimeData *mime = event->mimeData();
  if (mime->hasFormat(QString::fromLatin1(kMediaMime)) || mime->hasFormat(QString::fromLatin1(kAlbumMime)) || mime->hasUrls()) event->acceptProposedAction();
}
void AlbumTree::dragMoveEvent(QDragMoveEvent *event) {
  const QMimeData *mime = event->mimeData();
  const QPoint pos = event->position().toPoint();
  const int row = rowAt(pos);
  const bool album = mime->hasFormat(QString::fromLatin1(kAlbumMime));
  const bool media = mime->hasFormat(QString::fromLatin1(kMediaMime));
  QString target = row >= 0 && !rows_[row].favorites ? rows_[row].id : QString();
  if (album) {
    const QString moving = QString::fromUtf8(mime->data(QString::fromLatin1(kAlbumMime)));
    // An album never lands on itself or inside its own subtree; empty space means the library root.
    if (target == moving || target.startsWith(moving + QLatin1Char('/'))) {
      event->ignore();
      clearDrop();
      return;
    }
    event->acceptProposedAction();
  } else if (!target.isEmpty()) {
    event->setDropAction(media ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
  } else {
    event->ignore();
    clearDrop();
    return;
  }
  if (target != dropTarget_) {
    dropTarget_ = target;
    hoverExpandTarget_ = target;
    if (row >= 0 && rows_[row].hasChildren && !expanded_.contains(target)) hoverExpand_.start();
    else hoverExpand_.stop();
    viewport()->update();
  }
  QString hint;
  const QString name = row >= 0 ? rows_[row].name : t("album.libraryRoot");
  if (media) hint = I18n::n("drag.moveTo", QString::fromLatin1(mime->data(QString::fromLatin1(kMediaMime))).toInt(), {{"album", name}});
  else if (album) hint = t("album.moveTo") + QStringLiteral(" ") + name;
  else hint = t("drag.addTo", {{"album", name}});
  emit dragHint(hint, viewport()->mapToGlobal(pos));
}
void AlbumTree::dragLeaveEvent(QDragLeaveEvent *) { clearDrop(); }
void AlbumTree::dropEvent(QDropEvent *event) {
  const QMimeData *mime = event->mimeData();
  const int row = rowAt(event->position().toPoint());
  const QString target = row >= 0 && !rows_[row].favorites ? rows_[row].id : QString();
  clearDrop();
  if (mime->hasFormat(QString::fromLatin1(kAlbumMime))) {
    event->acceptProposedAction();
    emit albumDropped(QString::fromUtf8(mime->data(QString::fromLatin1(kAlbumMime))), target);
  } else if (!target.isEmpty() && mime->hasFormat(QString::fromLatin1(kMediaMime))) {
    event->setDropAction(Qt::MoveAction);
    event->accept();
    emit mediaDropped(target);
  } else if (!target.isEmpty() && mime->hasUrls()) {
    QStringList paths;
    for (const QUrl &url : mime->urls())
      if (url.isLocalFile()) paths << url.toLocalFile();
    event->setDropAction(Qt::CopyAction);
    event->accept();
    if (!paths.isEmpty()) emit filesDropped(paths, target);
  }
}
VirtualListSource AlbumTree::accessibleSource() {
  VirtualListSource source;
  source.count = [this] { return int(rows_.size()); };
  source.name = [this](int i) { return i < rows_.size() ? rows_[i].name : QString(); };
  source.description = [this](int i) {
    if (i >= rows_.size()) return QString();
    const Row &row = rows_[i];
    QString text = QString::number(row.count);
    if (row.hasChildren) text += QStringLiteral(", ") + (expanded_.contains(row.id) ? t("album.collapse") : t("album.expand"));
    return text;
  };
  source.rect = [this](int i) {
    const QRect r = rowRect(i).intersected(viewport()->rect());
    return r.isEmpty() ? QRect() : QRect(viewport()->mapToGlobal(r.topLeft()), r.size());
  };
  source.selected = [this](int i) { return i < rows_.size() && rows_[i].id == active_; };
  source.activate = [this](int i) {
    if (i < rows_.size()) emit activated(rows_[i].id);
  };
  return source;
}
