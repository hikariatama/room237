#pragma once
// Virtualized gallery. Paints only visible tiles from a bounded image cache on the raster path.
// Uniform grids need no per-item geometry; masonry, Apple-style and grouped views use the
// full ordered projection from the core.
#include "accessibility.h"
#include "model.h"
#include "thumbs.h"
#include "widgets.h"
#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantAnimation>
struct FrameRecording {
  std::vector<double> times, ys, wheel;
  struct Sample { double t; int visible, ready, expected, blank; double y; };
  std::vector<Sample> samples;
  QHash<QString, double> first, readyAt;
};
class GalleryView : public QAbstractScrollArea {
  Q_OBJECT
public:
  enum Layout { Grid, Masonry, Apple };
  GalleryView(GalleryModel *model, ThumbPipeline *pipeline, QWidget *parent = nullptr);
  void setColumns(int columns);
  void setLayoutKind(Layout layout);
  Layout layoutKind() const { return layout_; }
  void setFrameCap(int milliseconds);
  int columns() const { return columns_; }
  // Selection is index-based with an explicit select-all state.
  int selectionCount() const;
  bool isSelected(int index) const;
  void clearSelection();
  void selectAll();
  void collectSelection(std::function<void(QVector<MediaItem>)> done);
  QRect tileRect(int index) const;  // viewport coordinates
  int tileRadius() const;
  int indexAt(QPoint point) const;
  struct Visible { int visible = 0, ready = 0, expected = 0, blank = 0; };
  Visible visibleState(QVector<QPair<QString, bool>> *keys = nullptr);
  void startRecording();
  FrameRecording stopRecording();
  bool recording() const { return recording_; }
  double tilePoints() const;
  int visibleIndexInGrid(int offset) const;
  int focusIndexValue() const { return focus_; }
  std::pair<int, int> visibleRange(int extraPixels = 0) const;
  void setEmptyHint(const QString &title, const QString &subtitle, const QByteArray &icon);
  void setBlocked(bool blocked);  // Privacy: no input, no opening, no native drag.
  void focusIndex(int index);
  int focusedIndex() const { return focus_; }
  bool nonUniform() const;
  VirtualListSource accessibleSource();
signals:
  void dragFinished();
  void openRequested(int index);
  void selectionChanged();
  void contextRequested(int index, QPoint global);
  void filesDropped(QStringList paths);
  void copyRequested(int index);
  void deleteRequested(int index);
  void favoriteRequested(int index);
protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void scrollContentsBy(int dx, int dy) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dragLeaveEvent(QDragLeaveEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void focusInEvent(QFocusEvent *event) override;
  void focusOutEvent(QFocusEvent *event) override;
  bool viewportEvent(QEvent *event) override;
private:
  struct Group { QString name; qreal y = 0, end = 0; };
  enum Hit { HitNone, HitFavorite, HitCopy, HitDelete, HitConfirmDelete, HitConfirmCancel };
  void relayout();
  void rebuildGeometry();
  void updateDemand();
  void startDrag(int index);
  void noteFrame();
  void paintTile(QPainter &painter, const QRect &rect, int index, const QImage *picture);
  const QImage *pictureFor(int index);
  void paintExtras(QPainter &painter, const QRect &rect, int index);
  void paintCorners(QPainter &painter, const QRect &rect);
  void captureAnchor();
  void restoreAnchor();
  void paintDateBadge(QPainter &painter, const QRect &rect, const MediaItem &item);
  void paintScrollIndicator(QPainter &painter);
  void paintGroups(QPainter &painter);
  void paintEmpty(QPainter &painter);
  void setHovered(int index, QPoint position);
  Hit hitAt(int index, QPoint position) const;
  QRect actionRect(const QRect &tile, Hit hit) const;
  QString cacheKey(const MediaItem &item, const QSize &tileDevicePixels) const;
  QSize deviceSizeFor(int index) const;
  int devicePixels() const;
  int layoutWidth() const;
  void rebuildChrome();
  double gap() const;
  double step() const;
  void ensureVisible(int index);
  void selectRangeTo(int index);
  void toggleIndex(int index);
  void scrollBy(qreal delta);
  // Tiles grow under the pointer and shrink while pressed, like 6.x's framer-motion tiles (spring 600/25).
  void setTileTarget(int index, qreal target);
  qreal tileScale(int index) const;
  void stepSprings();
  void setHoverHit(Hit hit);
  QRect springRect(int index) const;
  QHash<int, Spring> springs_;
  Spring hitSpring_;
  Hit hoverHit_ = HitNone;
  QTimer springTick_;
  QElapsedTimer springClock_;
  qint64 springLast_ = 0;
  GalleryModel *model_;
  ThumbPipeline *pipeline_;
  int columns_ = 7, focus_ = -1, anchor_ = -1, pressIndex_ = -1, hovered_ = -1, confirm_ = -1;
  Layout layout_ = Grid;
  QPoint pressPos_;
  bool pressHandled_ = false;
  bool all_ = false;
  QSet<int> selected_, excluded_;
  QVector<QRectF> rects_;
  QVector<Group> groups_;
  qreal contentHeight_ = 0, maxTileHeight_ = 0;
  QTimer demandTimer_;
  bool recording_ = false;
  double pendingWheel_ = -1;
  QElapsedTimer recordClock_;
  FrameRecording rec_;
  QTimer sampler_;
  QTimer frameCap_;
  bool framePending_ = false;
  int capMs_ = 0;
  bool draggingThumb_ = false, blocked_ = false, dropActive_ = false;
  int visibleReadyPainted_ = -1;
  QString emptyTitle_, emptySubtitle_;
  QByteArray emptyIcon_;
  bool emptyFrame_ = false;
  int anchorIndex_ = -1, anchorOffset_ = 0;
  QPixmap corners_[4];
  qreal cornersRatio_ = 0;
  int cornersRadius_ = 0;
  QPixmap topFade_, bottomFade_;
  QVariantAnimation hoverFade_, indicatorFade_, smoothScroll_;
  qreal hoverAlpha_ = 0, indicatorAlpha_ = 0;
  QTimer indicatorHide_, scrollIdle_;
  bool scrolling_ = false;
  qreal wheelTarget_ = -1;
};
double uptimeMs();
QRect scrollThumbRect(int viewportWidth, int viewportHeight, int value, int maximum);
void paintScrollThumb(QPainter &painter, const QRect &thumb, qreal alpha = 1);
qint64 envMegabytes(const char *name, qint64 fallback);
bool envFlag(const char *name, bool fallback);
QString formatMediaDate(double seconds, bool compact);
