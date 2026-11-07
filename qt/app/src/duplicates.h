#pragma once
// Virtualized duplicate groups on top of the core's disk index.
#include "gallery.h"
#include "model.h"
#include "thumbs.h"
#include "widgets.h"
#include <QAbstractScrollArea>
#include <QCache>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
struct DuplicateGroup {
  QString root;
  int count = 0;
  QVector<MediaItem> items;
};
// Preview of one file over the workspace: name and position on top, arrows at the sides.
class DuplicatePreview : public QWidget {
  Q_OBJECT
public:
  explicit DuplicatePreview(QWidget *parent);
  void showItems(const QVector<MediaItem> &items, int index, const QPixmap &frost);
  int index() const { return index_; }
  bool ready() const { return !image_.isNull(); }
  QString currentName() const { return items_.isEmpty() ? QString() : items_[index_].name; }
  void retranslate();
signals:
  void closed();
protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void keyPressEvent(QKeyEvent *event) override;
  void resizeEvent(QResizeEvent *) override;
private:
  void step(int delta);
  void load();
  void finish();
  QVector<MediaItem> items_;
  int index_ = 0, generation_ = 0;
  QImage image_;
  QPixmap frost_;
  qreal enter_ = 1;
  QVariantAnimation enterAnimation_;
  IconButton *close_, *previous_, *next_;
};
class DuplicatesView : public QAbstractScrollArea {
  Q_OBJECT
public:
  DuplicatesView(CoreClient *core, ThumbPipeline *pipeline, QWidget *parent = nullptr);
  void open(const QString &directory, std::function<void()> done = {});
  // After an action on one group: the list is replaced when the answer is in, without emptying it first. `affected`
  // is the group acted on; when it is gone, the groups below move up into its place.
  void refresh(int affected, std::function<void()> done = {});
  void clear();
  void retranslate();  // Labels, tooltips and cached text pictures after a language change.
  // Fades in from the page and rises by 8px without repainting window-sized layers per step.
  void setEntrance(qreal progress);
  int total() const { return total_; }
  int filesTotal() const { return filesTotal_; }
  QString signature() const { return signature_; }
  bool loading() const { return loading_; }
  void markDifferent(int group, std::function<void()> done = {});
  void keepFirst(int group, std::function<void(QString)> done = {});
  void keepOnly(int group, const MediaItem &keep, std::function<void(QString)> done = {});
  void deleteOne(int group, const MediaItem &item, std::function<void(QString)> done = {});
  void deleteAll(std::function<void(int)> done = {});
  GalleryView::Visible visibleState(QVector<QPair<QString, bool>> *keys = nullptr);
  const DuplicateGroup *group(int index) const;
  void startRecording();
  FrameRecording stopRecording();
  void ensureGroup(int group, std::function<void()> done);
  QRect tileGlobalRect(int group, int item) const;
  QJsonObject previewState() const;
  QRect markDifferentRect(int group) const;
signals:
  void changed();
  void mutationStarted();
  void mutationFinished();  // Album contents changed; the gallery has to catch up.
  void closeRequested();
  void previewRequested(int group, int item, QVector<MediaItem> members);
  void failure(QString message);
protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void scrollContentsBy(int, int) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void enterEvent(QEnterEvent *) override;
  void leaveEvent(QEvent *) override;
  void keyPressEvent(QKeyEvent *event) override;
private:
  struct Hover { int group = -1, item = -1, action = 0; };
  QRect thumbRect() const;
  void dragThumb(int y);
  // Where the pointer holds the scroll thumb, from its top; -1 while it is not dragged.
  int thumbGrab_ = -1;
  QVector<MediaItem> stripItems(int group) const;
  void loadMembers(const DuplicateGroup &group);
  void preview(int group, int item);
  int stripOffset(int group) const;
  void ensureChrome();
  QString tileKey(const MediaItem &item, bool kept) const;
  void paintTile(QPainter &painter, int group, int index, const MediaItem &item, bool kept, const QVector<MediaItem> &strip);
  void loadPage(int offset);
  void relayout();
  void updateDemand();
  void noteFrame();
  void layoutFooter();
  Hover hitAt(QPoint point) const;
  QRect tileRect(int group, int item) const;
  int visibleTiles(int group) const;
  int rowHeight() const { return 228; }
  int rowTop(int group) const;  // Viewport y of a group's card, including the move-up after a removal.
  void startCollapse(int from);
  static constexpr int kTile = 128, kStep = 136;
  CoreClient *core_;
  ThumbPipeline *pipeline_;
  QString directory_, signature_;
  QHash<int, DuplicateGroup> groups_;
  QSet<int> loadingOffsets_;
  QHash<QString, QVector<MediaItem>> members_;
  QSet<QString> loadingMembers_;
  QHash<QString, int> stripX_;
  QPixmap cardCorners_[4], tileCorners_[4], keptCorners_[4], bestBadge_, differentLabel_;
  QImage tileCornerImages_[4], keptCornerImages_[4];
  mutable int markDifferentWidth_ = 0;
  QCache<QString, QPixmap> captions_;
  qreal entrance_ = 1;
  int rise_ = 0;
  QTimer hideScroll_;
  Glide actions_, confirmFade_;
  DuplicatePreview *preview_;
  TextButton *backButton_;
  int total_ = 0, filesTotal_ = 0, pageSize_ = 0, generation_ = 0;
  bool loading_ = false, busy_ = false;
  int pendingGroup_ = -1, collapseFrom_ = -1;
  qreal collapseShift_ = 0;
  QVariantAnimation collapse_;
  Hover hover_, confirm_;
  QTimer demandTimer_, sampler_;
  bool recording_ = false;
  double pendingWheel_ = -1;
  QElapsedTimer recordClock_;
  FrameRecording rec_;
  QWidget *footer_;
  QLabel *summary_;
  TextButton *deleteAllButton_;
  Spinner *spinner_;
};
