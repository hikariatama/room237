#pragma once
// Album tree: drawn rows over the core's flat album list, with drag and drop in both directions.
#include "accessibility.h"
#include "model.h"
#include "widgets.h"
#include <QAbstractScrollArea>
#include <QHash>
#include <QSet>
#include <QThreadPool>
inline const QString kFavoritesId = QStringLiteral("__favorites__");
class AlbumTree : public QAbstractScrollArea {
  Q_OBJECT
public:
  explicit AlbumTree(QWidget *parent = nullptr);
  ~AlbumTree() override;
  void setAlbums(const QVector<AlbumInfo> &albums, int favorites);
  void setActive(const QString &id);
  QString active() const { return active_; }
  QRect rectFor(const QString &albumId) const;
  bool isExpanded(const QString &id) const { return expanded_.contains(id); }
  // Opens the ancestors of id and, with self, id itself.
  void expandTo(const QString &id, bool self = false);
  int focusRowIndex() const { return focusRow_; }
  void collapseAutoExpandedExcept(const QString &keep);
  void setExpanded(const QString &id, bool expanded);
  void setBlocked(bool blocked);
  VirtualListSource accessibleSource();
  int rowCount() const { return rows_.size(); }
  QString rowId(int index) const { return index >= 0 && index < rows_.size() ? rows_[index].id : QString(); }
  QSize sizeHint() const override { return {235, 400}; }
  void setInternalDragCount(int count) { internalDrag_ = count; }
signals:
  void activated(QString albumId);
  void contextRequested(QString albumId, QPoint global);
  void mediaDropped(QString albumId);
  void filesDropped(QStringList paths, QString albumId);
  void albumDropped(QString albumId, QString parentId);
  void newAlbumRequested(QRect anchorInTree);
  void dragHint(QString text, QPoint global);
protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dragLeaveEvent(QDragLeaveEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void scrollContentsBy(int, int) override { viewport()->update(); }
private:
  struct Row {
    QString id, name, path, thumb;
    int depth = 0, count = 0;
    bool hasChildren = false, favorites = false;
  };
  void rebuildRows();
  int rowAt(QPoint point) const;
  int rowIndex(const QString &id) const;
  QRect rowRect(int row) const;
  QRect newAlbumRect() const;
  QRect chevronRect(int row) const;
  void updateScroll();
  int contentMax() const;
  void layoutRows();
  void fadeHover(int row);
  qreal appearing(int row) const;
  void clearDrop();
  void loadIcon(const QString &thumb);
  QVector<AlbumInfo> albums_;
  QVector<Row> rows_;
  QSet<QString> expanded_, manuallyExpanded_;
  QString active_, dropTarget_, hoverExpandTarget_;
  int favorites_ = 0, hovered_ = -1, pressed_ = -1, focusRow_ = -1, internalDrag_ = 0;
  bool hoveredNew_ = false, blocked_ = false;
  QPoint pressPos_;
  QTimer hoverExpand_;
  QHash<QString, QImage> icons_;
  QSet<QString> iconsLoading_;
  QThreadPool iconPool_;
  // Rows that were not there before grow in (6.x: height and opacity over 150 ms) and push the rows below down.
  QSet<QString> knownIds_, appearingIds_;
  QVariantAnimation appear_;
  // A floor under the scroll range after rows folded away at the end of the list; -1 when none.
  int slackMax_ = -1;
  qreal appearProgress_ = 1;
  QVector<int> tops_;
  QVariantAnimation hoverAnim_;
  qreal hoverT_ = 1;
  int fadingOut_ = -1;  // Content y of each row and, last, of the new-album button.
};
