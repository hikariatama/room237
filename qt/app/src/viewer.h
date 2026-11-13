#pragma once
// Full-window viewer. Originals decode on a small owned pool; the newest request wins.
#include "model.h"
#include "widgets.h"
#include <QAudioOutput>
#include <QLabel>
#include <QMediaPlayer>
#include <QThreadPool>
#include <QJsonObject>
#include <QVideoWidget>
#include <QWidget>
#include <atomic>
#include <QIODevice>
#include <memory>
class SeekBar;
class Viewer : public QWidget {
  Q_OBJECT
public:
  Viewer(QWidget *parent = nullptr);
  ~Viewer() override;
  // origin: where the tile sits in viewer coordinates, for the opening motion; preview: its current picture.
  void showItem(const MediaItem &item, const QRect &origin = {}, const QImage &preview = {});
  void closeViewer();
  void beginClose();
  bool isOpen() const { return open_; }
  bool mediaReady() const { return ready_; }
  bool failed() const { return failed_; }
  QString currentPath() const { return open_ ? item_.path() : QString(); }
  const MediaItem &item() const { return item_; }
  QJsonObject videoChromeState() const;
#ifdef ROOM237_QA
  void wakeForQa() { wakeChrome(); }
#endif
  void windowMoved() { if (open_ && item_.isVideo) placeVideoBar(); }
  bool isVideo() const { return item_.isVideo; }
  bool isAnimated() const { return animation_ != nullptr; }
  qint64 animationFrames() const { return animationFrames_; }
  QMediaPlayer *player() const { return player_; }
  qint64 videoFrames() const { return videoFrames_; }
  double zoom() const { return zoom_; }
  void setZoom(double zoom);
  void setFavorite(bool favorite);
  void retranslate();  // Labels and tooltips after a language change.
  void setPosition(int index, int total);
  void setCanNavigate(bool previous, bool next);
  // Hidden, minimized or privacy-locked: nothing decodes, plays or stays readable behind the shield.
  void setSuspended(bool suspended);
  bool suspended() const { return suspended_; }
  void setBackdrop(const QPixmap &backdrop) { backdrop_ = backdrop; }
  // Where the picture flies from and back to: the tile on screen and its corner radius.
  void setOrigin(const QRect &origin, qreal radius) {
    origin_ = origin;
    originRadius_ = radius;
  }
  void setFrost(const QPixmap &frost) { frost_ = frost; }
  void prefetch(const MediaItem &item);
  void keepDecoded(const QString &path, const QImage &fit);
signals:
  void navigate(int delta);
  // Before the close animation: the place to fly back to can still be set.
  void closing();
  void closed();
  void becameReady();
  void favoriteRequested(MediaItem item);
  void copyRequested(MediaItem item);
  void deleteRequested(MediaItem item);
  void revealRequested(MediaItem item);
protected:
  void paintEvent(QPaintEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  bool event(QEvent *event) override;
  bool eventFilter(QObject *object, QEvent *event) override;
private:
  struct Animation;
  void delivered(int generation, QImage fit, QSize original);
  void ensureFull();
  void releasePlayer();
  void startVideo();
  void startDecode();
  void startAnimation();
  void animationStep();
  void requestAnimationFrame();
  void layoutChrome();
  void placeVideoBar();
  void wakeChrome();
  void hideChrome();
  void setChromeVisible(bool visible);
  void showFailure();
  void hideFailure();
  QSize decodeBox() const;
  QRectF mediaArea() const;
  QRectF fitRect(QSizeF content) const;
  QRectF imageRect() const;
  void zoomAt(double target, QPointF focal);
  void clampPan();
  void applyVideoGeometry();
  MediaItem item_;
  bool open_ = false, ready_ = false, dragging_ = false, failed_ = false, moved_ = false, suspended_ = false, resumeVideo_ = false, closing_ = false, fading_ = false;
  std::atomic<int> generation_{0};
  QImage full_, fit_, preview_;
  QSize originalSize_;
  int fullPending_ = -1;
  double zoom_ = 1;
  QPointF pan_, lastPos_, pressPos_;
  QMediaPlayer *player_ = nullptr;
  QAudioOutput *audio_ = nullptr;
  QVideoWidget *video_ = nullptr;
  qint64 videoFrames_ = 0, animationFrames_ = 0;
  int index_ = -1, total_ = 0;
  QHash<QString, QImage> prefetched_;
  QStringList prefetchOrder_;
  QRect origin_;
  qreal originRadius_ = 0;
  QPixmap backdrop_, frost_;
  QVariantAnimation openAnimation_, chromeFade_;
  qreal open01_ = 1, chrome01_ = 1;
  bool chromeShown_ = true;
  IconButton *closeButton_, *favoriteButton_, *copyButton_, *deleteButton_, *previousButton_, *nextButton_, *fullscreenButton_;
  QWidget *topBar_, *videoBar_;
  IconButton *playButton_, *muteButton_, *backButton_, *forwardButton_;
  SeekBar *seek_, *volume_;
  QLabel *timeLabel_, *durationLabel_, *titleLabel_, *subtitleLabel_;
  QWidget *failureBox_;
  QTimer chromeTimer_;
  QTimer pointerPoll_;  // A native video surface does not report plain pointer motion to Qt.
  QPoint lastPointer_;
  QLabel *failureText_;
  TextButton *retryButton_, *failureRevealButton_;
  std::unique_ptr<Animation> animation_;
  std::unique_ptr<QIODevice> sealedVideo_;  // Decrypted stream of the open video in an encrypted root.
  QTimer animationTimer_;
  bool savedMuted_ = false;
  float savedVolume_ = 1;
  QThreadPool decoders_;  // Declared last: workers use the members above, the destructor waits for them.
};
