#include "duplicates.h"
#include <QtMath>
#include <array>
#include <functional>
#include "i18n.h"
#include "panels.h"
#include "thumbs.h"
#include <cmath>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLocale>
#include <QThreadPool>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>
// The cards take the gallery's columns: the same left edge, the same 20-point gutter for the scroll bar on the right,
// and 13 points of padding inside.
static constexpr int kFooter = 48, kCardGap = 8, kGutter = 20, kStripTop = 13, kStripHeight = 174, kStripLeft = 13;
static constexpr int kMemberPage = 256;
enum { ActionNone, ActionKeep, ActionDelete, ActionConfirm, ActionCancel, ActionPreview, ActionDifferent };
DuplicatePreview::DuplicatePreview(QWidget *parent) : QWidget(parent) {
  setAttribute(Qt::WA_OpaquePaintEvent);
  setFocusPolicy(Qt::StrongFocus);
  hide();
  close_ = new IconButton("x", t("common.close"), 32, this);
  previous_ = new IconButton("chevron-left", t("media.previous"), 36, this);
  next_ = new IconButton("chevron-right", t("media.next"), 36, this);
  for (IconButton *button : {previous_, next_}) {
    button->setIconSize(20);
    button->setRadius(18);
    button->setColors(Qt::white, Qt::white, QColor(255, 255, 255, 51));
    button->setBaseFill(QColor(255, 255, 255, 26));
  }
  connect(close_, &QAbstractButton::clicked, this, &DuplicatePreview::finish);
  connect(previous_, &QAbstractButton::clicked, this, [this] { step(-1); });
  connect(next_, &QAbstractButton::clicked, this, [this] { step(1); });
  enterAnimation_.setStartValue(0.0);
  enterAnimation_.setEndValue(1.0);
  enterAnimation_.setDuration(240);
  enterAnimation_.setEasingCurve(Theme::bezier(0, 0, 0.58, 1));
  connect(&enterAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    enter_ = v.toReal();
    update();
  });
}
void DuplicatePreview::showItems(const QVector<MediaItem> &items, int index, const QPixmap &frost) {
  items_ = items;
  index_ = index;
  frost_ = frost;
  setGeometry(parentWidget()->rect());
  show();
  raise();
  setFocus();
  load();
}
void DuplicatePreview::retranslate() {
  close_->setLabel(t("common.close"));
  previous_->setLabel(t("media.previous"));
  next_->setLabel(t("media.next"));
}
void DuplicatePreview::finish() {
  hide();
  generation_++;
  image_ = {};
  frost_ = {};
  emit closed();
}
void DuplicatePreview::step(int delta) {
  const int next = index_ + delta;
  if (next < 0 || next >= items_.size() || items_[next].isVideo) return;
  index_ = next;
  load();
}
void DuplicatePreview::load() {
  previous_->setVisible(index_ > 0);
  next_->setVisible(index_ + 1 < items_.size());
  image_ = {};
  const int version = ++generation_;
  const QString path = items_[index_].path();
  QPointer<DuplicatePreview> self(this);
  QThreadPool::globalInstance()->start([self, path, version] {
    const QImage decoded = decodeOriginal(path);
    if (!self) return;
    QMetaObject::invokeMethod(self.data(), [self, version, decoded] {
      if (!self || version != self->generation_) return;
      self->image_ = decoded;
      self->enter_ = 0;
      self->enterAnimation_.stop();
      self->enterAnimation_.setDuration(Theme::motion(240));
      self->enterAnimation_.start();
      self->update();
    });
  });
  update();
}
void DuplicatePreview::resizeEvent(QResizeEvent *) {
  close_->move(width() - close_->width() - 12, 12);
  previous_->move(12, 56 + (height() - 56 - 24 - previous_->height()) / 2);
  next_->move(width() - next_->width() - 12, 56 + (height() - 56 - 24 - next_->height()) / 2);
}
void DuplicatePreview::paintEvent(QPaintEvent *) {
  QPainter p(this);
  if (!frost_.isNull()) p.drawPixmap(0, 0, frost_);
  p.fillRect(rect(), QColor(0, 0, 0, 217));
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  if (items_.isEmpty()) return;
  p.setPen(Theme::text);
  p.setFont(Theme::font(14, QFont::Medium));
  p.drawText(QRect(16, 12, width() - 72, 20), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(items_[index_].label(), Qt::ElideMiddle, width() - 72));
  p.setPen(Theme::muted);
  p.setFont(Theme::font(12));
  p.drawText(QRect(16, 32, width() - 72, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%1 / %2").arg(index_ + 1).arg(items_.size()));
  if (image_.isNull()) return;
  // Never upscaled.
  const QRectF area(56, 56, width() - 112, height() - 56 - 24);
  QSizeF size = QSizeF(image_.size());
  if (size.width() > area.width() || size.height() > area.height()) size.scale(area.size(), Qt::KeepAspectRatio);
  const qreal scale = 0.985 + 0.015 * enter_;
  QRectF target(QPointF(0, 0), size * scale);
  target.moveCenter(area.center());
  QPainterPath clip;
  clip.addRoundedRect(target, Theme::kRadiusXl, Theme::kRadiusXl);
  p.setOpacity(enter_);
  p.setClipPath(clip);
  p.drawImage(target, image_);
}
void DuplicatePreview::mousePressEvent(QMouseEvent *) { finish(); }
void DuplicatePreview::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) finish();
  else if (event->key() == Qt::Key_Left) step(-1);
  else if (event->key() == Qt::Key_Right) step(1);
  else QWidget::keyPressEvent(event);
}
DuplicatesView::DuplicatesView(CoreClient *core, ThumbPipeline *pipeline, QWidget *parent) : QAbstractScrollArea(parent), core_(core), pipeline_(pipeline), captions_(160), actions_(viewport(), 150), confirmFade_(viewport(), 160) {
  setFrameShape(QFrame::NoFrame);
  setViewportMargins(4, 4, 0, 0);  // Level with the gallery's tiles.
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  viewport()->setMouseTracking(true);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(t("duplicates.title"));
  demandTimer_.setSingleShot(true);
  demandTimer_.setInterval(0);
  connect(&demandTimer_, &QTimer::timeout, this, &DuplicatesView::updateDemand);
  connect(pipeline_, &ThumbPipeline::ready, this, [this] { if (isVisible()) viewport()->update(); });
  connect(pipeline_, &ThumbPipeline::retry, this, [this] { if (isVisible()) demandTimer_.start(); });
  sampler_.setInterval(100);
  connect(&sampler_, &QTimer::timeout, this, [this] {
    QVector<QPair<QString, bool>> keys;
    const auto v = visibleState(&keys);
    const double now = recordClock_.nsecsElapsed() / 1e6;
    rec_.samples.push_back({now, v.visible, v.ready, v.expected, v.blank, double(verticalScrollBar()->value())});
    for (const auto &[key, ok] : keys) {
      if (!rec_.first.contains(key)) rec_.first.insert(key, now);
      if (ok && !rec_.readyAt.contains(key)) rec_.readyAt.insert(key, now);
    }
  });
  footer_ = new QWidget(viewport());
  auto *row = new QHBoxLayout(footer_);
  row->setContentsMargins(4, 8, 4, 8);
  row->setSpacing(12);
  summary_ = new QLabel(footer_);
  summary_->setFont(Theme::font(12));
  summary_->setStyleSheet(QStringLiteral("color:#636363;background:transparent"));
  spinner_ = new Spinner(14, footer_);
  deleteAllButton_ = new TextButton(t("duplicates.deleteAll"), TextButton::Danger, footer_);
  deleteAllButton_->setSmall(true);
  deleteAllButton_->setFixedHeight(32);
  deleteAllButton_->setIcon("trash");
  row->addStretch();
  row->addWidget(spinner_);
  row->addWidget(summary_);
  row->addWidget(deleteAllButton_);
  footer_->hide();
  connect(deleteAllButton_, &QAbstractButton::clicked, this, [this] {
    QWidget *host = window()->findChild<QWidget *>(QStringLiteral("overlayHost"));
    if (!host) host = window();
    auto *panel = new ConfirmPanel(host, t("duplicates.deleteAllTitle"), t("duplicates.deleteAllConfirm"), t("common.delete"), [this] { deleteAll(); });
    panel->setFixedPanelWidth(320);
    panel->openAt(hostRect(host, deleteAllButton_), Qt::TopEdge, Qt::AlignRight);
  });
  backButton_ = new TextButton(t("duplicates.backToAlbum"), TextButton::Outline, viewport());
  backButton_->setSmall(true);
  backButton_->hide();
  connect(backButton_, &QAbstractButton::clicked, this, &DuplicatesView::closeRequested);
  collapse_.setDuration(Theme::motion(220));
  collapse_.setEasingCurve(QEasingCurve::OutCubic);
  connect(&collapse_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    collapseShift_ = v.toReal();
    viewport()->update();
  });
  connect(&collapse_, &QVariantAnimation::finished, this, [this] {
    collapseFrom_ = -1;
    collapseShift_ = 0;
    viewport()->update();
  });
  preview_ = new DuplicatePreview(this);
  connect(preview_, &DuplicatePreview::closed, this, [this] { setFocus(); });
}
static DuplicateGroup groupFromJson(const QJsonObject &o, const QString &directory) {
  DuplicateGroup g;
  g.root = o.value(QLatin1String("root")).toString();
  g.count = o.value(QLatin1String("count")).toInt();
  for (const auto &row : o.value(QLatin1String("items")).toArray()) {
    MediaItem m = mediaFromJson(row.toObject());
    if (m.albumPath.isEmpty()) m.albumPath = directory;
    g.items.push_back(std::move(m));
  }
  return g;
}
void DuplicatesView::clear() {
  generation_++;
  groups_.clear();
  members_.clear();
  captions_.clear();
  bestBadge_ = differentLabel_ = QPixmap();
  markDifferentWidth_ = 0;  // The language may have changed since the last review.
  loadingMembers_.clear();
  stripX_.clear();
  loadingOffsets_.clear();
  total_ = filesTotal_ = 0;
  signature_.clear();
  hover_ = confirm_ = {};
  verticalScrollBar()->setValue(0);
  footer_->hide();
  relayout();
}
void DuplicatesView::retranslate() {
  setAccessibleName(t("duplicates.title"));
  deleteAllButton_->setText(t("duplicates.deleteAll"));
  backButton_->setText(t("duplicates.backToAlbum"));
  preview_->retranslate();
  bestBadge_ = differentLabel_ = QPixmap();
  markDifferentWidth_ = 0;
  captions_.clear();  // Dates follow the language.
  layoutFooter();
  viewport()->update();
}
void DuplicatesView::setEntrance(qreal progress) {
  entrance_ = progress;
  rise_ = qRound((1 - progress) * 8);
  move(x(), rise_);
  viewport()->update();
}
void DuplicatesView::open(const QString &directory, std::function<void()> done) {
  clear();
  directory_ = directory;
  loading_ = true;
  layoutFooter();
  viewport()->update();
  const int version = generation_;
  core_->call(QStringLiteral("find_duplicate_groups"), {{QStringLiteral("dir"), directory}, {QStringLiteral("offset"), 0}, {QStringLiteral("signature"), QJsonValue()}}, [this, version, done = std::move(done)](const QJsonValue &result, const QString &error) {
    if (version != generation_) return;
    loading_ = false;
    if (!error.isEmpty()) {
      qWarning("duplicates: %s", qPrintable(error));
      emit failure(error);
    }
    const QJsonObject o = result.toObject();
    total_ = o.value(QLatin1String("total")).toInt();
    filesTotal_ = o.value(QLatin1String("filesTotal")).toInt();
    signature_ = o.value(QLatin1String("signature")).toString();
    const QJsonArray rows = o.value(QLatin1String("groups")).toArray();
    pageSize_ = qMax(1, rows.size());
    for (int i = 0; i < rows.size(); i++) groups_.insert(i, groupFromJson(rows[i].toObject(), directory_));
    relayout();
    layoutFooter();
    emit changed();
    if (done) done();
  });
}
void DuplicatesView::refresh(int affected, std::function<void()> done) {
  const int version = ++generation_;
  const int before = total_;
  const QString affectedRoot = group(affected) ? group(affected)->root : QString();
  loadingOffsets_.clear();
  loadingMembers_.clear();
  core_->call(QStringLiteral("find_duplicate_groups"), {{QStringLiteral("dir"), directory_}, {QStringLiteral("offset"), 0}, {QStringLiteral("signature"), QJsonValue()}}, [this, version, before, affected, affectedRoot, done = std::move(done)](const QJsonValue &result, const QString &error) {
    if (version != generation_) return;
    if (!error.isEmpty()) {
      qWarning("duplicates: %s", qPrintable(error));
      emit failure(error);
    }
    const QJsonObject o = result.toObject();
    const int scroll = verticalScrollBar()->value();
    const QHash<int, DuplicateGroup> previous = std::exchange(groups_, {});
    members_.remove(affectedRoot);
    total_ = o.value(QLatin1String("total")).toInt();
    filesTotal_ = o.value(QLatin1String("filesTotal")).toInt();
    signature_ = o.value(QLatin1String("signature")).toString();
    const QJsonArray rows = o.value(QLatin1String("groups")).toArray();
    pageSize_ = qMax(1, int(rows.size()));
    for (int i = 0; i < rows.size(); i++) groups_.insert(i, groupFromJson(rows[i].toObject(), directory_));
    // Groups past the first page stay on screen; only the one acted on changed, and a removed one shifts the rest.
    const bool removed = total_ == before - 1;
    for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
      if (it.key() == affected) continue;
      const int index = removed && it.key() > affected ? it.key() - 1 : it.key();
      if (index < total_ && !groups_.contains(index)) groups_.insert(index, it.value());
    }
    hover_ = confirm_ = {};
    pendingGroup_ = -1;
    relayout();
    verticalScrollBar()->setValue(qMin(scroll, verticalScrollBar()->maximum()));
    if (removed && affected >= 0 && affected < total_) startCollapse(affected);
    layoutFooter();
    viewport()->update();
    emit changed();
    if (done) done();
  });
}
int DuplicatesView::rowTop(int g) const {
  return g * rowHeight() - verticalScrollBar()->value() + (collapseFrom_ >= 0 && g >= collapseFrom_ ? qRound(collapseShift_) : 0);
}
void DuplicatesView::startCollapse(int from) {
  if (!Theme::motion(220)) return;
  collapseFrom_ = from;
  collapse_.stop();
  collapse_.setStartValue(qreal(rowHeight()));
  collapse_.setEndValue(0.0);
  collapse_.start();
}
void DuplicatesView::layoutFooter() {
  const bool show = total_ > 0;
  footer_->setVisible(show);
  footer_->setGeometry(0, total_ * rowHeight() - verticalScrollBar()->value(), viewport()->width() - kGutter, kFooter);
  spinner_->setVisible(show && (loading_ || busy_));
  summary_->setText(t("duplicates.updating"));
  summary_->setVisible(show && (loading_ || busy_));
  deleteAllButton_->setVisible(total_ > 0);
  deleteAllButton_->setEnabled(!loading_ && !busy_);
  footer_->raise();
}
void DuplicatesView::loadPage(int offset) {
  if (loadingOffsets_.contains(offset) || groups_.contains(offset) || offset >= total_) return;
  loadingOffsets_.insert(offset);
  const int version = generation_;
  core_->call(QStringLiteral("find_duplicate_groups"), {{QStringLiteral("dir"), directory_}, {QStringLiteral("offset"), offset}, {QStringLiteral("signature"), signature_}}, [this, version, offset](const QJsonValue &result, const QString &error) {
    if (version != generation_) return;
    loadingOffsets_.remove(offset);
    if (!error.isEmpty()) {
      qWarning("duplicates page: %s", qPrintable(error));
      return;
    }
    const QJsonArray rows = result.toObject().value(QLatin1String("groups")).toArray();
    for (int i = 0; i < rows.size(); i++) groups_.insert(offset + i, groupFromJson(rows[i].toObject(), directory_));
    // Retain a bounded window of decoded group DTOs.
    const int top = verticalScrollBar()->value() / rowHeight();
    for (auto it = groups_.begin(); it != groups_.end();) it = qAbs(it.key() - top) > 200 ? groups_.erase(it) : std::next(it);
    viewport()->update();
    demandTimer_.start();
  });
}
void DuplicatesView::relayout() {
  const int visible = viewport()->height();
  verticalScrollBar()->setPageStep(visible);
  verticalScrollBar()->setSingleStep(40);
  verticalScrollBar()->setRange(0, qMax(0, total_ * rowHeight() + kFooter - visible));
  viewport()->update();
  demandTimer_.start();
}
void DuplicatesView::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  if (preview_->isVisible()) preview_->setGeometry(rect());
  relayout();
  layoutFooter();
}
void DuplicatesView::scrollContentsBy(int, int) {
  footer_->move(0, total_ * rowHeight() - verticalScrollBar()->value());
  viewport()->update();
  demandTimer_.start();
  hover_ = {};
}
void DuplicatesView::wheelEvent(QWheelEvent *event) {
  QPoint delta = event->pixelDelta();
  if (delta.isNull()) delta = event->angleDelta() / 2;
  if (recording_ && pendingWheel_ < 0) pendingWheel_ = double(event->timestamp());
  // A strip scrolls sideways on its own; the list keeps the vertical axis.
  if (qAbs(delta.x()) > qAbs(delta.y())) {
    const Hover h = hitAt(event->position().toPoint());
    if (h.group >= 0) {
      if (const DuplicateGroup *group = this->group(h.group)) {
        const int count = group->count;
        const int maximum = qMax(0, count * kStep - 8 - (viewport()->width() - kGutter - 2 * kStripLeft));
        const int next = qBound(0, stripX_.value(group->root) - delta.x(), maximum);
        stripX_.insert(group->root, next);
        if (members_.value(group->root).size() < count && count <= kMemberPage * 8) loadMembers(*group);
        viewport()->update();
        demandTimer_.start();
      }
    }
    event->accept();
    return;
  }
  verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
  event->accept();
}
const DuplicateGroup *DuplicatesView::group(int index) const {
  auto it = groups_.constFind(index);
  return it == groups_.cend() ? nullptr : &*it;
}
QVector<MediaItem> DuplicatesView::stripItems(int g) const {
  const DuplicateGroup *group = this->group(g);
  if (!group) return {};
  const auto loaded = members_.constFind(group->root);
  return loaded != members_.cend() && loaded->size() > group->items.size() ? *loaded : group->items;
}
void DuplicatesView::loadMembers(const DuplicateGroup &group) {
  if (loadingMembers_.contains(group.root)) return;
  const QString root = group.root;
  const int offset = members_.value(root).size();
  const int version = generation_;
  loadingMembers_.insert(root);
  core_->call(QStringLiteral("duplicate_group_members"), {{QStringLiteral("dir"), directory_}, {QStringLiteral("root"), root}, {QStringLiteral("signature"), signature_}, {QStringLiteral("offset"), offset}}, [this, root, version](const QJsonValue &result, const QString &) {
    if (version != generation_) return;
    loadingMembers_.remove(root);
    QVector<MediaItem> &members = members_[root];
    for (const auto &row : result.toArray()) {
      MediaItem m = mediaFromJson(row.toObject());
      if (m.albumPath.isEmpty()) m.albumPath = directory_;
      members.push_back(std::move(m));
    }
    viewport()->update();
    demandTimer_.start();
  });
}
void DuplicatesView::preview(int g, int index) {
  const QVector<MediaItem> items = stripItems(g);
  if (index < 0 || index >= items.size()) return;
  if (items[index].isVideo) {
    emit previewRequested(g, index, items);
    return;
  }
  QVector<MediaItem> images;
  int position = 0;
  for (int i = 0; i < items.size(); i++) {
    if (items[i].isVideo) continue;
    if (i == index) position = images.size();
    images.push_back(items[i]);
  }
  preview_->showItems(images, position, Theme::frostedCopy(this, rect(), 12));
}
QRect DuplicatesView::tileGlobalRect(int g, int item) const {
  const QRect r = tileRect(g, item);
  return QRect(viewport()->mapToGlobal(r.topLeft()), r.size());
}
QJsonObject DuplicatesView::previewState() const {
  return {{QStringLiteral("visible"), preview_->isVisible()}, {QStringLiteral("ready"), preview_->ready()}, {QStringLiteral("index"), preview_->index()}, {QStringLiteral("name"), preview_->currentName()}};
}
int DuplicatesView::stripOffset(int g) const {
  const DuplicateGroup *group = this->group(g);
  return group ? stripX_.value(group->root) : 0;
}
int DuplicatesView::visibleTiles(int g) const {
  const int strip = viewport()->width() - kGutter - 2 * kStripLeft;
  return qMax(1, (stripOffset(g) + strip) / kStep + 1);
}
QRect DuplicatesView::tileRect(int g, int item) const { return {kStripLeft + item * kStep - stripOffset(g), rowTop(g) + kStripTop, kTile, kTile}; }
QRect DuplicatesView::markDifferentRect(int g) const {
  if (!markDifferentWidth_) markDifferentWidth_ = QFontMetrics(Theme::font(12, QFont::Medium)).horizontalAdvance(t("duplicates.markNonDuplicates")) + 16 + 6 + 20;
  const int w = markDifferentWidth_;
  // Its label ends 16 points from the card's corner; the hover pill keeps 6 points of it.
  return {viewport()->width() - kGutter - 6 - w, rowTop(g) + rowHeight() - kCardGap - 9 - 28, w, 28};
}
static QRect keepRect(const QRect &tile) { return {tile.right() + 1 - 6 - 28 - 4 - 28, tile.bottom() + 1 - 6 - 28, 28, 28}; }
static QRect removeRect(const QRect &tile) { return {tile.right() + 1 - 6 - 28, tile.bottom() + 1 - 6 - 28, 28, 28}; }
// Confirm buttons, centred under the text.
static void confirmRects(const QRect &tile, const QString &yes, const QString &no, QRect *accept, QRect *cancel) {
  const QFontMetrics semibold(Theme::font(11, QFont::DemiBold)), medium(Theme::font(11, QFont::Medium));
  const int w1 = semibold.horizontalAdvance(yes) + 16, w2 = medium.horizontalAdvance(no) + 16;
  const int x = tile.left() + (tile.width() - w1 - 4 - w2) / 2, y = tile.center().y() + 2;
  *accept = QRect(x, y, w1, 22);
  *cancel = QRect(x + w1 + 4, y, w2, 22);
}
// Four corner pieces: the outside of the arc in a flat colour close to what is behind it, and the arc itself.
static void makeCorners(QPixmap out[4], int radius, qreal ratio, const QColor &outside, const QColor &arc, qreal arcWidth) {
  const int side = qCeil(radius * ratio);
  QImage base(side, side, QImage::Format_ARGB32_Premultiplied);
  base.fill(Qt::transparent);
  {
    QPainter p(&base);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath area, round;
    area.addRect(0, 0, side, side);
    round.addEllipse(0, 0, side * 2, side * 2);
    p.fillPath(area.subtracted(round), outside);
    const qreal w = arcWidth * ratio;
    p.setPen(QPen(arc, w));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(w / 2, w / 2, side * 2 - w, side * 2 - w));
  }
  out[0] = QPixmap::fromImage(base);
  out[1] = QPixmap::fromImage(base.flipped(Qt::Horizontal));
  out[2] = QPixmap::fromImage(base.flipped(Qt::Vertical));
  out[3] = QPixmap::fromImage(base.flipped(Qt::Horizontal | Qt::Vertical));
  for (int i = 0; i < 4; i++) out[i].setDevicePixelRatio(ratio);
}
static void paintFrame(QPainter &painter, const QRect &rect, const QPixmap corners[4], int radius, const QColor &line, int width) {
  const qreal size = corners[0].width() / corners[0].devicePixelRatio();
  const QRect top(rect.left() + radius, rect.top(), rect.width() - 2 * radius, width), left(rect.left(), rect.top() + radius, width, rect.height() - 2 * radius);
  painter.fillRect(top, line);
  painter.fillRect(top.translated(0, rect.height() - width), line);
  painter.fillRect(left, line);
  painter.fillRect(left.translated(rect.width() - width, 0), line);
  painter.drawPixmap(QPointF(rect.left(), rect.top()), corners[0]);
  painter.drawPixmap(QPointF(rect.right() + 1 - size, rect.top()), corners[1]);
  painter.drawPixmap(QPointF(rect.left(), rect.bottom() + 1 - size), corners[2]);
  painter.drawPixmap(QPointF(rect.right() + 1 - size, rect.bottom() + 1 - size), corners[3]);
}
// The ring and the rounded corners, burned into the picture: a tile then costs one opaque copy per frame.
static void frameThumbnail(QImage &image, const QImage corners[4], const QColor &line, int width) {
  QPainter p(&image);
  const int w = image.width(), h = image.height(), side = corners[0].width();
  p.fillRect(side, 0, w - 2 * side, width, line);
  p.fillRect(side, h - width, w - 2 * side, width, line);
  p.fillRect(0, side, width, h - 2 * side, line);
  p.fillRect(w - width, side, width, h - 2 * side, line);
  p.drawImage(0, 0, corners[0]);
  p.drawImage(w - side, 0, corners[1]);
  p.drawImage(0, h - side, corners[2]);
  p.drawImage(w - side, h - side, corners[3]);
}
QString DuplicatesView::tileKey(const MediaItem &item, bool kept) const {
  return item.path() + QLatin1Char('#') + QString::number(qRound(kTile * devicePixelRatioF())) + (kept ? QLatin1Char('k') : QLatin1Char('f'));
}
void DuplicatesView::ensureChrome() {
  const qreal ratio = viewport()->devicePixelRatioF();
  if (!cardCorners_[0].isNull() && cardCorners_[0].devicePixelRatio() == ratio) return;
  const QColor pageAverage(21, 20, 20), cardAverage(27, 26, 26);
  makeCorners(cardCorners_, Theme::kRadiusXl, ratio, pageAverage, QColor(255, 255, 255, 20), 1);
  makeCorners(tileCorners_, Theme::kRadiusLg, ratio, cardAverage, QColor(255, 255, 255, 26), 1);
  makeCorners(keptCorners_, Theme::kRadiusLg, ratio, cardAverage, QColor(0, 212, 146, 153), 2);
  for (int i = 0; i < 4; i++) {
    // Device pixels, for drawing into a thumbnail.
    tileCornerImages_[i] = tileCorners_[i].toImage();
    tileCornerImages_[i].setDevicePixelRatio(1);
    keptCornerImages_[i] = keptCorners_[i].toImage();
    keptCornerImages_[i].setDevicePixelRatio(1);
  }
}
static QPixmap textPixmap(const QSize &size, qreal ratio, const std::function<void(QPainter &)> &paint) {
  QPixmap out(size * ratio);
  out.setDevicePixelRatio(ratio);
  out.fill(Qt::transparent);
  QPainter painter(&out);
  painter.setRenderHint(QPainter::Antialiasing);
  paint(painter);
  return out;
}
void DuplicatesView::paintTile(QPainter &painter, int g, int index, const MediaItem &item, bool kept, const QVector<MediaItem> &strip) {
  const QRect rect = tileRect(g, index);
  const qreal ratio = devicePixelRatioF();
  const QString key = tileKey(item, kept);
  QPainterPath clip;
  const bool confirming = confirm_.group == g && confirm_.item == index;
  const bool hovered = hover_.group == g && hover_.item == index && !busy_;
  const QImage *picture = pipeline_->get(key);
  if (!picture || picture->hasAlphaChannel()) painter.fillRect(rect, QColor(255, 255, 255, 13));
  if (picture) {
    painter.drawImage(rect, *picture);
  } else {
    // Ring, then the Best badge. A decoded tile carries its own.
    paintFrame(painter, rect, kept ? keptCorners_ : tileCorners_, Theme::kRadiusLg, kept ? QColor(0, 212, 146, 153) : QColor(255, 255, 255, 26), kept ? 2 : 1);
  }
  if (item.isVideo) Theme::drawIcon(painter, "player-play-filled", QRectF(rect.center().x() - 10, rect.center().y() - 10, 20, 20), Qt::white);
  if (!confirming && hovered) {
    painter.setOpacity(actions_.value());
    painter.setPen(Qt::NoPen);
    const bool canKeep = strip.size() > 1 && strip.size() >= stripItems(g).size() && this->group(g)->count <= kMemberPage;
    if (canKeep) {
      const QRect keep = keepRect(rect);
      painter.setBrush(QColor(0, 0, 0, 153));
      painter.drawRoundedRect(keep, Theme::kRadiusLg, Theme::kRadiusLg);
      Theme::drawIcon(painter, "star", QRectF(keep).adjusted(6, 6, -6, -6), hover_.action == ActionKeep ? QColor(94, 233, 181) : QColor(255, 255, 255, 230));
    }
    const QRect remove = removeRect(rect);
    painter.setBrush(QColor(0, 0, 0, 153));
    painter.drawRoundedRect(remove, Theme::kRadiusLg, Theme::kRadiusLg);
    Theme::drawIcon(painter, "trash", QRectF(remove).adjusted(6, 6, -6, -6), hover_.action == ActionDelete ? Theme::red400 : QColor(255, 255, 255, 230));
    painter.setOpacity(1);
  }
  if (kept) {
    if (bestBadge_.isNull()) {
      const QFont font = Theme::font(10, QFont::DemiBold);
      const QString label = t("duplicates.kept");
      const int w = QFontMetrics(font).horizontalAdvance(label) + 12 + 4 + 12;
      bestBadge_ = textPixmap(QSize(w, 19), ratio, [&](QPainter &p) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 188, 125, 217));
        p.drawRoundedRect(QRect(0, 0, w, 19), 9.5, 9.5);
        Theme::drawIcon(p, "star-filled", QRectF(6, 3.5, 12, 12), Qt::white);
        p.setPen(Qt::white);
        p.setFont(font);
        p.drawText(QRect(22, 0, w - 22, 19), Qt::AlignVCenter | Qt::AlignLeft, label);
      });
    }
    painter.drawPixmap(QPointF(rect.left() + 8, rect.top() + 8), bestBadge_);
  }
  if (confirming) {
    // Above the ring and the badge, in the tile's clip.
    clip.addRoundedRect(QRectF(rect), Theme::kRadiusLg, Theme::kRadiusLg);
    painter.save();
    painter.setClipPath(clip, Qt::IntersectClip);
    painter.setOpacity(confirmFade_.value());
    if (const QImage *image = pipeline_->get(key)) {
      const QImage small = image->scaled(image->size() / 12, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
      painter.drawImage(rect, small);
    }
    painter.fillRect(rect, QColor(0, 0, 0, 166));
    painter.setPen(Qt::white);
    painter.setFont(Theme::font(11, QFont::Medium));
    painter.drawText(rect.adjusted(8, 8, -8, -rect.height() / 2 - 4), Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, confirm_.action == ActionKeep ? t("duplicates.keepOnlyConfirm") : t("duplicates.deleteFileConfirm"));
    const QString yes = confirm_.action == ActionKeep ? t("duplicates.keep") : t("common.delete");
    QRect accept, cancel;
    confirmRects(rect, yes, t("common.cancel"), &accept, &cancel);
    const bool yesHot = hover_.action == ActionConfirm && hover_.group == g && hover_.item == index;
    const bool noHot = hover_.action == ActionCancel && hover_.group == g && hover_.item == index;
    QColor fill = confirm_.action == ActionKeep ? QColor(0, 150, 105) : Theme::red500;
    fill.setAlphaF(yesHot ? 1 : 0.8);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(accept, Theme::kRadiusSm, Theme::kRadiusSm);
    painter.setBrush(QColor(255, 255, 255, noHot ? 51 : 26));
    painter.drawRoundedRect(cancel, Theme::kRadiusSm, Theme::kRadiusSm);
    painter.setPen(Qt::white);
    painter.setFont(Theme::font(11, QFont::DemiBold));
    painter.drawText(accept, Qt::AlignCenter, yes);
    painter.setFont(Theme::font(11, QFont::Medium));
    painter.drawText(cancel, Qt::AlignCenter, t("common.cancel"));
    painter.setOpacity(1);
    painter.restore();
  }
  QPixmap *caption = captions_.object(item.path());
  if (!caption) {
    QStringList parts;
    if (item.width > 0 && item.height > 0) parts << QStringLiteral("%1×%2").arg(item.width).arg(item.height);
    if (const double when = item.displayDate(); when > 0) parts << QLocale(I18n::language() == QLatin1String("ru") ? QLocale::Russian : QLocale::English).toString(QDateTime::fromSecsSinceEpoch(qint64(when)).date(), QLocale::ShortFormat);
    caption = new QPixmap(textPixmap(QSize(kTile, 28), ratio, [&](QPainter &p) {
      const QFont nameFont = Theme::font(11, QFont::Medium), metaFont = Theme::font(10);
      p.setFont(nameFont);
      p.setPen(Theme::text);
      p.drawText(QRect(0, 0, kTile, 14), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(nameFont).elidedText(item.label(), Qt::ElideRight, kTile));
      p.setFont(metaFont);
      p.setPen(Theme::muted);
      p.drawText(QRect(0, 15, kTile, 13), Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(metaFont).elidedText(parts.join(QStringLiteral(" · ")), Qt::ElideRight, kTile));
    }));
    captions_.insert(item.path(), caption);
  }
  painter.drawPixmap(QPointF(rect.left(), rect.bottom() + 7), *caption);
}
void DuplicatesView::paintEvent(QPaintEvent *event) {
  QPainter painter(viewport());
  ensureChrome();
  // The page goes under the gaps; rise_ keeps its texture in place while the view moves in.
  const QPoint pageOffset = mapTo(window(), QPoint(0, -rise_));
  const auto page = [&](const QRegion &region, int whiteAlpha = 0) {
    for (const QRect &r : region) Theme::paintPage(painter, r, pageOffset, whiteAlpha);
  };
  const int row = rowHeight(), top = verticalScrollBar()->value();
  const int first = qMax(0, top / row - (collapseFrom_ >= 0 ? 1 : 0)), last = qMin(total_ - 1, (top + viewport()->height()) / row);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.setRenderHint(QPainter::Antialiasing);
  const int listHeight = viewport()->height();
  const int cardWidth = viewport()->width() - kGutter;
  backButton_->setVisible(!loading_ && total_ == 0);
  if (total_ == 0) {
    page(event->region());
    const QPoint center(cardWidth / 2, viewport()->height() / 2 - 20);
    if (loading_) {
      painter.save();
      painter.translate(center.x(), center.y() - 32);
      painter.rotate(fmod(QDateTime::currentMSecsSinceEpoch() / 1000.0 * 360, 360));
      Theme::drawIcon(painter, "loader-2", QRectF(-12, -12, 24, 24), Theme::muted);
      painter.restore();
      QTimer::singleShot(33, viewport(), qOverload<>(&QWidget::update));
    } else {
      Theme::drawIcon(painter, "circle-check", QRectF(center.x() - 14, center.y() - 46, 28, 28), Theme::emerald400);
    }
    painter.setPen(Theme::text);
    painter.setFont(Theme::font(14, QFont::Medium));
    painter.drawText(QRect(0, center.y() - 6, cardWidth, 20), Qt::AlignCenter, loading_ ? t("duplicates.loading") : t("duplicates.none"));
    painter.setPen(Theme::muted);
    painter.setFont(Theme::font(12));
    painter.drawText(QRect(0, center.y() + 18, cardWidth, 18), Qt::AlignCenter, loading_ ? t("duplicates.loadingHint") : t("duplicates.noneHint"));
    backButton_->move((cardWidth - backButton_->sizeHint().width()) / 2, center.y() + 48);
    backButton_->resize(backButton_->sizeHint());
    if (entrance_ < 1) {
      painter.setOpacity(1 - entrance_);
      page(event->region());
    }
    noteFrame();
    return;
  }
  // The page goes under the gaps, its tinted copy under the cards, and neither under an opaque tile.
  QRegion cards;
  QRegion covered;
  for (int g = first; g <= last; g++) {
    const int y = rowTop(g);
    cards += QRect(0, y, cardWidth, row - kCardGap);
    const DuplicateGroup *group = this->group(g);
    if (!group) continue;
    const QRect stripArea(kStripLeft, y + kStripTop, cardWidth - 2 * kStripLeft, kStripHeight);
    const QVector<MediaItem> strip = stripItems(g);
    for (int i = stripOffset(g) / kStep, to = qMin<int>(strip.size(), visibleTiles(g) + 1); i < to; i++) {
      const QImage *picture = pipeline_->get(tileKey(strip[i], strip[i].name == group->root));
      if (picture && !picture->hasAlphaChannel()) covered += tileRect(g, i).intersected(stripArea);
    }
  }
  const QRegion damage = event->region();
  page(damage - cards);
  page((damage & cards) - covered, 6);
  for (int g = first; g <= last; g++) {
    const int y = rowTop(g);
    // Already mixed over the card: this long line is a plain fill, not a blend per row.
    paintFrame(painter, QRect(0, y, cardWidth, row - kCardGap), cardCorners_, Theme::kRadiusXl, QColor(45, 44, 44), 1);
    const DuplicateGroup *group = this->group(g);
    if (!group) continue;
    const QVector<MediaItem> strip = stripItems(g);
    painter.save();
    // The group being changed waits dimmed for the new list.
    if (g == pendingGroup_) painter.setOpacity(0.45);
    painter.setClipRect(QRect(kStripLeft, y + kStripTop, cardWidth - 2 * kStripLeft, kStripHeight));
    const int scroll = stripOffset(g);
    const int from = scroll / kStep, to = qMin<int>(strip.size(), visibleTiles(g) + 1);
    for (int i = from; i < to; i++) paintTile(painter, g, i, strip[i], strip[i].name == group->root, strip);
    if (group->count > strip.size() && to == strip.size()) {
      const QRect more = tileRect(g, strip.size());
      QPen dashed(QColor(255, 255, 255, 26), 1, Qt::DashLine);
      painter.setPen(dashed);
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(QRectF(more).adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRadiusLg, Theme::kRadiusLg);
      painter.setPen(Theme::muted);
      painter.setFont(Theme::font(12));
      painter.drawText(more.adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, I18n::n("duplicates.moreFiles", group->count - strip.size()));
    }
    painter.restore();
    const QRect different = markDifferentRect(g);
    if (hover_.group == g && hover_.action == ActionDifferent) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(Theme::hover);
      painter.drawRoundedRect(different, Theme::kRadiusXl, Theme::kRadiusXl);
    }
    if (differentLabel_.isNull()) {
      differentLabel_ = textPixmap(different.size(), devicePixelRatioF(), [&](QPainter &p) {
        Theme::drawIcon(p, "arrows-split-2", QRectF(10, different.height() / 2.0 - 8, 16, 16), Theme::text);
        p.setPen(Theme::text);
        p.setFont(Theme::font(12, QFont::Medium));
        p.drawText(QRect(32, 0, different.width() - 42, different.height()), Qt::AlignVCenter | Qt::AlignLeft, t("duplicates.markNonDuplicates"));
      });
    }
    painter.drawPixmap(QPointF(different.topLeft()), differentLabel_);
  }
  // The same fades as the gallery while there is more to scroll.
  const auto fade = [&](bool atTop) {
    const QRect band(0, atTop ? 0 : listHeight - 48, viewport()->width(), 48);
    QLinearGradient gradient(0, atTop ? band.top() : band.bottom(), 0, atTop ? band.bottom() : band.top());
    gradient.setColorAt(0, Theme::sidebar);
    gradient.setColorAt(1, QColor(0x15, 0x14, 0x14, 0));
    painter.fillRect(band, gradient);
  };
  if (top > 0) fade(true);
  if (top < verticalScrollBar()->maximum()) fade(false);
  if (underMouse() || thumbGrab_ >= 0) paintScrollThumb(painter, scrollThumbRect(viewport()->width(), listHeight, top, verticalScrollBar()->maximum()));
  if (entrance_ < 1) {
    painter.setOpacity(1 - entrance_);
    page(event->region());
  }
  noteFrame();
}
DuplicatesView::Hover DuplicatesView::hitAt(QPoint point) const {
  Hover h;
  if (total_ <= 0 || point.x() >= viewport()->width() - kGutter) return h;
  const int g = (point.y() + verticalScrollBar()->value()) / rowHeight();
  if (g < 0 || g >= total_) return h;
  const DuplicateGroup *group = this->group(g);
  if (!group) return h;
  h.group = g;
  if (markDifferentRect(g).contains(point)) {
    h.action = ActionDifferent;
    return h;
  }
  const QVector<MediaItem> strip = stripItems(g);
  const int scroll = stripOffset(g);
  for (int i = scroll / kStep, end = qMin<int>(strip.size(), visibleTiles(g) + 1); i < end; i++) {
    const QRect rect = tileRect(g, i);
    if (!rect.contains(point)) continue;
    h.item = i;
    if (confirm_.group == g && confirm_.item == i) {
      QRect accept, cancel;
      confirmRects(rect, confirm_.action == ActionKeep ? t("duplicates.keep") : t("common.delete"), t("common.cancel"), &accept, &cancel);
      if (accept.contains(point)) h.action = ActionConfirm;
      else if (cancel.contains(point)) h.action = ActionCancel;
      return h;
    }
    const bool canKeep = strip.size() > 1 && group->count <= kMemberPage;
    if (removeRect(rect).contains(point)) h.action = ActionDelete;
    else if (canKeep && keepRect(rect).contains(point)) h.action = ActionKeep;
    else h.action = ActionPreview;
    return h;
  }
  return h;
}
QRect DuplicatesView::thumbRect() const { return scrollThumbRect(viewport()->width(), viewport()->height(), verticalScrollBar()->value(), verticalScrollBar()->maximum()); }
void DuplicatesView::dragThumb(int y) {
  const QRect thumb = thumbRect();
  const int travel = viewport()->height() - thumb.height();
  if (travel <= 0) return;
  verticalScrollBar()->setValue(int(qint64(y - thumbGrab_) * verticalScrollBar()->maximum() / travel));
}
void DuplicatesView::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && thumbGrab_ >= 0) {
    thumbGrab_ = -1;
    viewport()->update();
  }
}
void DuplicatesView::mouseMoveEvent(QMouseEvent *event) {
  if (thumbGrab_ >= 0) return dragThumb(event->pos().y());
  const Hover h = hitAt(event->pos());
  if (h.group != hover_.group || h.item != hover_.item || h.action != hover_.action) {
    if (h.group != hover_.group || h.item != hover_.item) {
      actions_.set(0);
      actions_.to(1);
    }
    hover_ = h;
    viewport()->setCursor(h.action != ActionNone ? Qt::PointingHandCursor : Qt::ArrowCursor);
    viewport()->update();
  }
}
void DuplicatesView::enterEvent(QEnterEvent *) { viewport()->update(); }
void DuplicatesView::leaveEvent(QEvent *) {
  hover_ = {};
  viewport()->update();
}
void DuplicatesView::mousePressEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton) return;
  // The scroll bar lives in the right gutter: a press on the thumb holds it where it was taken, elsewhere it centres there.
  if (verticalScrollBar()->maximum() > 0 && event->pos().x() >= viewport()->width() - kGutter) {
    const QRect thumb = thumbRect();
    thumbGrab_ = thumb.contains(event->pos()) ? event->pos().y() - thumb.top() : thumb.height() / 2;
    dragThumb(event->pos().y());
    viewport()->update();
    return;
  }
  if (busy_ || collapseFrom_ >= 0) return;
  setFocus();
  const Hover h = hitAt(event->pos());
  if (h.group >= 0 && h.group == pendingGroup_) return;
  const DuplicateGroup *group = h.group >= 0 ? this->group(h.group) : nullptr;
  switch (h.action) {
    case ActionDifferent:
      markDifferent(h.group);
      break;
    case ActionKeep:
    case ActionDelete:
      confirm_ = h;
      confirmFade_.set(0);
      confirmFade_.to(1);
      viewport()->update();
      return;
    case ActionConfirm: {
      const QVector<MediaItem> strip = stripItems(h.group);
      if (!group || h.item >= strip.size()) break;
      const MediaItem item = strip[h.item];
      const int action = confirm_.action;
      confirm_ = {};
      if (action == ActionKeep) keepOnly(h.group, item);
      else deleteOne(h.group, item);
      break;
    }
    case ActionCancel:
      confirm_ = {};
      viewport()->update();
      break;
    case ActionPreview:
      if (group) preview(h.group, h.item);
      break;
    default: break;
  }
  if (confirm_.group >= 0 && h.action != ActionConfirm && h.action != ActionCancel) {
    confirm_ = {};
    viewport()->update();
  }
}
void DuplicatesView::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_PageDown || event->key() == Qt::Key_PageUp) verticalScrollBar()->setValue(verticalScrollBar()->value() + (event->key() == Qt::Key_PageDown ? 1 : -1) * verticalScrollBar()->pageStep());
  else QAbstractScrollArea::keyPressEvent(event);
}
void DuplicatesView::noteFrame() {
  if (!recording_) return;
  rec_.times.push_back(recordClock_.nsecsElapsed() / 1e6);
  rec_.ys.push_back(verticalScrollBar()->value());
  if (pendingWheel_ >= 0) {
    rec_.wheel.push_back(uptimeMs() - pendingWheel_);
    pendingWheel_ = -1;
  }
}
void DuplicatesView::updateDemand() {
  if (total_ <= 0) return;
  ensureChrome();
  const int row = rowHeight(), top = verticalScrollBar()->value();
  const int first = top / row, last = qMin(total_ - 1, (top + viewport()->height()) / row);
  const int px = qRound(kTile * devicePixelRatioF());
  std::vector<ThumbRequest> demand;
  const auto add = [&](int g) {
    if (g < 0 || g >= total_) return;
    const DuplicateGroup *group = this->group(g);
    if (!group) {
      loadPage(g - g % pageSize_);
      return;
    }
    const QVector<MediaItem> strip = stripItems(g);
    const int from = stripOffset(g) / kStep, to = qMin<int>(strip.size(), visibleTiles(g) + 2);
    for (int i = from; i < to; i++) {
      const bool kept = strip[i].name == group->root;
      const QImage *corners = kept ? keptCornerImages_ : tileCornerImages_;
      std::array<QImage, 4> pieces{corners[0], corners[1], corners[2], corners[3]};
      const QColor line = kept ? QColor(0, 212, 146, 153) : QColor(255, 255, 255, 26);
      const int width = (kept ? 2 : 1) * qRound(devicePixelRatioF());
      demand.push_back({tileKey(strip[i], kept), strip[i].thumbPath(), px, 0, [pieces, line, width](QImage &image) { frameThumbnail(image, pieces.data(), line, width); }});
    }
  };
  for (int g = first; g <= last; g++) add(g);
  for (int d = 1; d <= 10; d++) {
    add(last + d);
    add(first - d);
  }
  pipeline_->setDemand(std::move(demand));
}
GalleryView::Visible DuplicatesView::visibleState(QVector<QPair<QString, bool>> *keys) {
  GalleryView::Visible v;
  const int row = rowHeight(), top = verticalScrollBar()->value();
  const int px = qRound(kTile * devicePixelRatioF());
  const int first = top / row, last = qMin(total_ - 1, (top + viewport()->height()) / row);
  for (int g = first; g <= last; g++) {
    const DuplicateGroup *group = this->group(g);
    if (!group) {
      v.blank++;
      v.expected++;
      continue;
    }
    const int shown = qMin<int>(group->items.size(), visibleTiles(g));
    for (int n = stripOffset(g) / kStep; n < shown; n++) {
      const MediaItem &item = group->items[n];
      v.visible++;
      v.expected++;
      const bool ok = pipeline_->has(tileKey(item, item.name == group->root));
      if (ok) v.ready++;
      if (keys) keys->push_back({item.path(), ok});
    }
  }
  return v;
}
void DuplicatesView::startRecording() {
  rec_ = {};
  recordClock_.start();
  pendingWheel_ = -1;
  recording_ = true;
  sampler_.start();
}
FrameRecording DuplicatesView::stopRecording() {
  recording_ = false;
  sampler_.stop();
  return std::move(rec_);
}
void DuplicatesView::ensureGroup(int g, std::function<void()> done) {
  if (group(g) || g >= total_) {
    if (done) done();
    return;
  }
  loadPage(g - g % qMax(1, pageSize_));
  QTimer::singleShot(50, this, [this, g, done = std::move(done)] { ensureGroup(g, done); });
}
void DuplicatesView::markDifferent(int g, std::function<void()> done) {
  const DuplicateGroup *group = this->group(g);
  if (!group) return;
  const QString directory = directory_;
  // Only this group disappears: the scroll position and the other reviewed groups stay.
  pendingGroup_ = g;
  viewport()->update();
  core_->call(QStringLiteral("mark_duplicate_group_different"), {{QStringLiteral("dir"), directory}, {QStringLiteral("root"), group->root}, {QStringLiteral("signature"), signature_}, {QStringLiteral("offset"), QJsonValue()}}, [this, directory, g, done = std::move(done)](const QJsonValue &, const QString &error) mutable {
    if (!error.isEmpty()) emit failure(error);
    if (directory != directory_) {
      if (done) done();
      return;
    }
    refresh(g, std::move(done));
  });
}
void DuplicatesView::keepFirst(int g, std::function<void(QString)> done) {
  const DuplicateGroup *group = this->group(g);
  if (!group) return;
  keepOnly(g, group->items.value(0), std::move(done));
}
void DuplicatesView::keepOnly(int g, const MediaItem &keep, std::function<void(QString)> done) {
  const DuplicateGroup *group = this->group(g);
  if (!group) return;
  const QString directory = directory_, root = group->root, signature = signature_;
  const int count = group->count;
  if (preview_->isVisible()) preview_->hide();
  auto doomed = std::make_shared<QJsonArray>();
  auto step = std::make_shared<std::function<void(int)>>();
  busy_ = true;
  pendingGroup_ = g;
  viewport()->update();
  layoutFooter();
  emit mutationStarted();
  *step = [=, this](int offset) {
    core_->call(QStringLiteral("duplicate_group_members"), {{QStringLiteral("dir"), directory}, {QStringLiteral("root"), root}, {QStringLiteral("signature"), signature}, {QStringLiteral("offset"), offset}}, [=, this](const QJsonValue &result, const QString &) {
      const QJsonArray rows = result.toArray();
      for (const auto &row : rows) {
        const MediaItem m = mediaFromJson(row.toObject());
        if (m.name != keep.name) doomed->push_back(QJsonObject{{QStringLiteral("name"), m.name}, {QStringLiteral("revision"), m.revision}});
      }
      if (!rows.isEmpty() && offset + rows.size() < count) {
        (*step)(offset + rows.size());
        return;
      }
      // The core deletes only while the file that stays is still what the user saw.
      const QJsonArray retained{QJsonObject{{QStringLiteral("path"), keep.path()}, {QStringLiteral("revision"), keep.revision}}};
      core_->call(QStringLiteral("delete_media_batch"), {{QStringLiteral("albumPath"), directory}, {QStringLiteral("targets"), *doomed}, {QStringLiteral("observed"), retained}}, [=, this](const QJsonValue &result, const QString &callError) {
        QTimer::singleShot(0, [step] { *step = nullptr; });
        const QString error = callError.isEmpty() ? result.toObject().value(QLatin1String("error")).toString() : callError;
        if (!error.isEmpty()) {
          qWarning("keep: %s", qPrintable(error));
          emit failure(error);
        }
        if (directory != directory_) {
          busy_ = false;
          emit mutationFinished();
          if (done) done(keep.name);
          return;
        }
        refresh(g, [this, done, keep] {
          busy_ = false;
          layoutFooter();
          emit mutationFinished();
          if (done) done(keep.name);
        });
      });
    });
  };
  (*step)(0);
}
void DuplicatesView::deleteOne(int g, const MediaItem &item, std::function<void(QString)> done) {
  const DuplicateGroup *group = this->group(g);
  MediaItem anchor;
  if (group) {
    for (const MediaItem &member : group->items) {
      if (member.name != item.name) {
        anchor = member;
        break;
      }
    }
  }
  if (anchor.name.isEmpty()) {
    emit failure(t("media.displayFailed"));
    if (done) done(QStringLiteral("group changed"));
    return;
  }
  const QString directory = directory_;
  busy_ = true;
  pendingGroup_ = g;
  viewport()->update();
  layoutFooter();
  emit mutationStarted();
  const QJsonArray retained{QJsonObject{{QStringLiteral("path"), anchor.path()}, {QStringLiteral("revision"), anchor.revision}}};
  core_->call(QStringLiteral("delete_media_batch"), {{QStringLiteral("albumPath"), directory}, {QStringLiteral("targets"), QJsonArray{QJsonObject{{QStringLiteral("name"), item.name}, {QStringLiteral("revision"), item.revision}}}}, {QStringLiteral("observed"), retained}}, [this, directory, g, done = std::move(done)](const QJsonValue &result, const QString &callError) {
    const QString error = callError.isEmpty() ? result.toObject().value(QLatin1String("error")).toString() : callError;
    if (!error.isEmpty()) emit failure(error);
    if (directory != directory_) {
      busy_ = false;
      emit mutationFinished();
      if (done) done(error);
      return;
    }
    refresh(g, [this, done, error] {
      busy_ = false;
      layoutFooter();
      emit mutationFinished();
      if (done) done(error);
    });
  });
}
void DuplicatesView::deleteAll(std::function<void(int)> done) {
  const QString directory = directory_;
  busy_ = true;
  layoutFooter();
  emit mutationStarted();
  core_->call(QStringLiteral("delete_duplicate_groups"), {{QStringLiteral("dir"), directory}, {QStringLiteral("signature"), signature_}}, [this, directory, done = std::move(done)](const QJsonValue &result, const QString &error) {
    if (!error.isEmpty()) {
      qWarning("delete all: %s", qPrintable(error));
      emit failure(error);
    }
    const int deleted = result.toInt();
    open(directory, [this, done, deleted] {
      busy_ = false;
      layoutFooter();
      emit mutationFinished();
      if (done) done(deleted);
    });
  });
}
