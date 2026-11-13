#include "viewer.h"
#include "gallery.h"
#include "i18n.h"
#include "platform.h"
#include "qatrace.h"
#include "thumbs.h"
#include <QApplication>
#include <QAudioDevice>
#include <QFileInfo>
#include <QCursor>
#include <QWindow>
#include <QGraphicsOpacityEffect>
#include <QLinearGradient>
#include <QPainterPath>
#include "panels.h"
#include <QHBoxLayout>
#include <QBuffer>
#include <QImageReader>
#include "secure_media.h"
#include <QKeyEvent>
#include <QLocale>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMutex>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>
#include <QDateTime>
#include <cmath>
static constexpr double kMaxZoom = 8, kWheelStep = 0.03;
// A slightly underdamped spring (damping ratio 0.75) that settles by the end of the animation: about 3% overshoot.
static qreal springProgress(qreal t) {
  constexpr qreal zeta = 0.75, omega = 10;
  const qreal damped = omega * std::sqrt(1 - zeta * zeta);
  return 1 - std::exp(-zeta * omega * t) * (std::cos(damped * t) + zeta * omega / damped * std::sin(damped * t));
}
static constexpr int kBounds = 64, kMinimap = 75;
class SeekBar : public QWidget {
public:
  std::function<void(qreal)> moved;
  explicit SeekBar(const QString &label, QWidget *parent = nullptr) : QWidget(parent) {
    setFixedHeight(24);
    setAccessibleName(label);
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
  }
  void setValue(qreal value) {
    if (dragging_) return;
    if (!qFuzzyCompare(value_ + 1, value + 1)) {
      value_ = value;
      update();
    }
  }
  qreal value() const { return value_; }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal y = height() / 2.0, w = width() - 12;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 46));
    p.drawRoundedRect(QRectF(6, y - 2, w, 4), 2, 2);
    p.setBrush(Theme::primary);
    p.drawRoundedRect(QRectF(6, y - 2, w * value_, 4), 2, 2);
    p.setPen(hasFocus() ? QPen(QColor(255, 255, 255, 89), 2) : Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(6 + w * value_, y), 6, 6);
  }
  void mousePressEvent(QMouseEvent *event) override {
    dragging_ = true;
    fromX(event->position().x());
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    if (dragging_) fromX(event->position().x());
  }
  void mouseReleaseEvent(QMouseEvent *) override { dragging_ = false; }
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
      value_ = qBound<qreal>(0, value_ + (event->key() == Qt::Key_Left ? -0.05 : 0.05), 1);
      if (moved) moved(value_);
      update();
    } else {
      QWidget::keyPressEvent(event);
    }
  }
private:
  void fromX(qreal x) {
    value_ = qBound<qreal>(0, (x - 6) / qMax(1, width() - 12), 1);
    if (moved) moved(value_);
    update();
  }
  qreal value_ = 0;
  bool dragging_ = false;
};
// One reader at a time decodes the next frame on the pool; the timer shows it on schedule.
struct Viewer::Animation {
  std::shared_ptr<QBuffer> plain;  // The decrypted file of an encrypted root, read by `reader`.
  std::shared_ptr<QImageReader> reader;
  QImage ready, shown;
  int readyDelay = 100, generation = 0;
  bool pending = false;
  QSize box;
};
static QString clock(qint64 ms) {
  const qint64 s = qMax<qint64>(0, ms) / 1000;
  return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0')) : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}
// Top bar: a gradient over the page, file name on the left, a chrome pill on the right.
class ViewerTopBar : public QWidget {
public:
  using QWidget::QWidget;
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    QLinearGradient gradient(0, 0, 0, height());
    gradient.setColorAt(0, QColor(0, 0, 0, 128));
    gradient.setColorAt(1, QColor(0, 0, 0, 0));
    p.fillRect(rect(), gradient);
  }
};
class ViewerPill : public QWidget {
public:
  using QWidget::QWidget;
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 26), 1));
    p.setBrush(QColor(0, 0, 0, 89));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), height() / 2.0, height() / 2.0);
  }
};
class ViewerNavButton : public IconButton {
public:
  ViewerNavButton(const QByteArray &icon, const QString &label, QWidget *parent) : IconButton(icon, label, 44, parent) {
    setIconSize(20);
    setColors(QColor(255, 255, 255, 217), Qt::white, QColor(255, 255, 255, 31));
  }
protected:
  void paintEvent(QPaintEvent *event) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 26), 1));
    p.setBrush(QColor(0, 0, 0, 89));
    p.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
    p.end();
    IconButton::paintEvent(event);
  }
};
Viewer::Viewer(QWidget *parent) : QWidget(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setMouseTracking(true);
  decoders_.setMaxThreadCount(4);
  decoders_.setExpiryTimeout(2000);
  hide();
  topBar_ = new ViewerTopBar(this);
  auto *top = new QHBoxLayout(topBar_);
  top->setContentsMargins(96, 12, 16, 16);  // Keeps clear of the window controls.
  top->setSpacing(16);
  auto *titles = new QVBoxLayout;
  titles->setSpacing(0);
  titles->setContentsMargins(0, 2, 0, 0);
  titleLabel_ = new QLabel;
  titleLabel_->setFont(Theme::font(14, QFont::Medium));
  titleLabel_->setStyleSheet(QStringLiteral("color:#ffffff;background:transparent"));
  titleLabel_->setFixedHeight(20);
  subtitleLabel_ = new QLabel;
  subtitleLabel_->setFont(Theme::font(12));
  subtitleLabel_->setStyleSheet(QStringLiteral("color:rgba(255,255,255,179);background:transparent"));
  subtitleLabel_->setFixedHeight(16);
  titles->addWidget(titleLabel_);
  titles->addWidget(subtitleLabel_);
  titles->addStretch();
  top->addLayout(titles, 1);
  auto *pill = new ViewerPill(topBar_);
  auto *pillRow = new QHBoxLayout(pill);
  pillRow->setContentsMargins(2, 2, 2, 2);
  pillRow->setSpacing(2);
  const auto chrome = [&](const char *icon, const QString &label) {
    auto *button = new IconButton(icon, label, 36, pill);
    button->setIconSize(18);
    button->setColors(QColor(255, 255, 255, 217), Qt::white, QColor(255, 255, 255, 31));
    button->setPressScale(true);
    pillRow->addWidget(button);
    return button;
  };
  favoriteButton_ = chrome("heart", t("media.favorite"));
  copyButton_ = chrome("copy", t("media.copy"));
  deleteButton_ = chrome("trash", t("contextMenu.delete"));
  deleteButton_->setColors(QColor(255, 255, 255, 217), Theme::red400, QColor(255, 255, 255, 31));
  auto *separator = new QWidget(pill);
  separator->setFixedSize(1, 20);
  separator->setStyleSheet(QStringLiteral("background:rgba(255,255,255,38)"));
  pillRow->addSpacing(2);
  pillRow->addWidget(separator, 0, Qt::AlignVCenter);
  pillRow->addSpacing(2);
  closeButton_ = chrome("x", t("common.close"));
  top->addWidget(pill, 0, Qt::AlignTop);
  previousButton_ = new ViewerNavButton("chevron-left", t("media.previous"), this);
  nextButton_ = new ViewerNavButton("chevron-right", t("media.next"), this);
  // The video draws in its own native surface above the window's backing store, so the controls over it are a small
  // translucent panel window that follows the viewer; opening the controls does not take focus from the app.
  videoBar_ = new ViewerPill(this);
  videoBar_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
  videoBar_->setAttribute(Qt::WA_TranslucentBackground);
  videoBar_->setAttribute(Qt::WA_ShowWithoutActivating);
  videoBar_->setMouseTracking(true);
  videoBar_->installEventFilter(this);
  auto *bar = new QHBoxLayout(videoBar_);
  bar->setContentsMargins(6, 2, 8, 2);
  bar->setSpacing(8);
  playButton_ = new IconButton("player-pause-filled", t("video.pause"), 36, videoBar_);
  playButton_->setIconSize(18);
  playButton_->setColors(QColor(255, 255, 255, 217), Qt::white, QColor(255, 255, 255, 31));
  // ±10 s skips. PiP and AirPlay have no QtMultimedia counterpart.
  auto *back = backButton_ = new IconButton("rewind-backward-10", t("video.back10"), 36, videoBar_);
  auto *forward = forwardButton_ = new IconButton("rewind-forward-10", t("video.forward10"), 36, videoBar_);
  for (IconButton *skip : {back, forward}) {
    skip->setIconSize(18);
    skip->setColors(QColor(255, 255, 255, 217), Qt::white, QColor(255, 255, 255, 31));
  }
  const auto skipBy = [this](qint64 delta) {
    if (player_ && player_->duration() > 0) player_->setPosition(qBound<qint64>(0, player_->position() + delta, player_->duration()));
  };
  connect(back, &QAbstractButton::clicked, this, [skipBy] { skipBy(-10000); });
  connect(forward, &QAbstractButton::clicked, this, [skipBy] { skipBy(10000); });
  timeLabel_ = new QLabel(QStringLiteral("0:00"), videoBar_);
  durationLabel_ = new QLabel(QStringLiteral("0:00"), videoBar_);
  QFont timeFont = Theme::font(12);
  timeFont.setFeature("tnum", 1);
  for (QLabel *time : {timeLabel_, durationLabel_}) {
    time->setFont(timeFont);
    time->setStyleSheet(QStringLiteral("color:rgba(255,255,255,204);background:transparent"));
  }
  seek_ = new SeekBar(t("video.seek"), videoBar_);
  muteButton_ = new IconButton("volume", t("video.mute"), 36, videoBar_);
  muteButton_->setIconSize(18);
  muteButton_->setColors(QColor(255, 255, 255, 217), Qt::white, QColor(255, 255, 255, 31));
  volume_ = new SeekBar(t("video.volume"), videoBar_);
  volume_->setFixedWidth(72);
  volume_->setValue(1);
  fullscreenButton_ = new IconButton("arrows-maximize", t("viewer.fullscreen"), 36, videoBar_);
  fullscreenButton_->setIconSize(18);
  fullscreenButton_->setColors(QColor(255, 255, 255, 217), Qt::white, QColor(255, 255, 255, 31));
  connect(fullscreenButton_, &QAbstractButton::clicked, this, [this] {
    QWidget *top = window();
    top->isFullScreen() ? top->showNormal() : top->showFullScreen();
  });
  bar->addWidget(back);
  bar->addWidget(playButton_);
  bar->addWidget(forward);
  bar->addWidget(timeLabel_);
  bar->addWidget(seek_, 1);
  bar->addWidget(durationLabel_);
  bar->addWidget(muteButton_);
  bar->addWidget(volume_);
  bar->addWidget(fullscreenButton_);
  videoBar_->hide();
  failureBox_ = new QWidget(this);
  failureBox_->setObjectName(QStringLiteral("failure"));
  failureBox_->setStyleSheet(QStringLiteral("#failure{background:rgba(0,0,0,204);border-radius:14px;}"));
  auto *failure = new QVBoxLayout(failureBox_);
  failure->setContentsMargins(24, 24, 24, 24);
  failure->setSpacing(12);
  failureText_ = new QLabel(t("media.displayFailed"));
  failureText_->setWordWrap(true);
  failureText_->setAlignment(Qt::AlignCenter);
  failureText_->setFont(Theme::font(14));
  failureText_->setStyleSheet(QStringLiteral("color:#ffffff;background:transparent"));
  retryButton_ = new TextButton(t("common.retry"), TextButton::Outline);
  failureRevealButton_ = new TextButton(t("media.revealOriginal"), TextButton::Outline);
  for (TextButton *button : {retryButton_, failureRevealButton_}) {
    button->setRadius(Theme::kRadiusLg);
    button->setFont(Theme::font(14));
  }
  failure->addWidget(failureText_);
  failure->addWidget(retryButton_, 0, Qt::AlignHCenter);
  failure->addWidget(failureRevealButton_, 0, Qt::AlignHCenter);
  failureBox_->setFixedWidth(384);
  failureBox_->hide();
  connect(closeButton_, &QAbstractButton::clicked, this, &Viewer::beginClose);
  connect(favoriteButton_, &QAbstractButton::clicked, this, [this] { emit favoriteRequested(item_); });
  connect(copyButton_, &QAbstractButton::clicked, this, [this] { emit copyRequested(item_); });
  connect(deleteButton_, &QAbstractButton::clicked, this, [this] {
    auto *panel = new ConfirmPanel(parentWidget(), t("media.deleteConfirm"), t("contextMenu.deleteWarning"), t("common.delete"), [this] { emit deleteRequested(item_); });
    panel->setFixedPanelWidth(256);
    panel->setActionIcon("trash");
    deleteButton_->setHeld(true);
    connect(panel, &Popover::closed, deleteButton_, [this] { deleteButton_->setHeld(false); });
    panel->openAt(hostRect(parentWidget(), deleteButton_), Qt::BottomEdge, Qt::AlignRight);
  });
  connect(previousButton_, &QAbstractButton::clicked, this, [this] { emit navigate(-1); });
  connect(nextButton_, &QAbstractButton::clicked, this, [this] { emit navigate(1); });
  connect(retryButton_, &QAbstractButton::clicked, this, [this] {
    const MediaItem item = item_;
    hideFailure();
    showItem(item);
  });
  connect(failureRevealButton_, &QAbstractButton::clicked, this, [this] { emit revealRequested(item_); });
  connect(playButton_, &QAbstractButton::clicked, this, [this] {
    if (!player_) return;
    if (player_->playbackState() == QMediaPlayer::PlayingState) player_->pause();
    else player_->play();
  });
  connect(muteButton_, &QAbstractButton::clicked, this, [this] {
    savedMuted_ = !savedMuted_;
    if (audio_) audio_->setMuted(savedMuted_);
    muteButton_->setIcon(savedMuted_ ? "volume-off" : "volume");
    muteButton_->setLabel(savedMuted_ ? t("video.unmute") : t("video.mute"));
  });
  seek_->moved = [this](qreal value) {
    if (player_ && player_->duration() > 0) player_->setPosition(qint64(value * player_->duration()));
  };
  volume_->moved = [this](qreal value) {
    savedVolume_ = float(value);
    if (audio_) audio_->setVolume(savedVolume_);
  };
  chromeTimer_.setSingleShot(true);
  chromeTimer_.setInterval(2200);
  connect(&chromeTimer_, &QTimer::timeout, this, [this] { setChromeVisible(false); });
  // Both ways run forward on the same spring: opening goes 0 to 1, closing 1 to 0, and each overshoots a little at its end.
  QEasingCurve spring(QEasingCurve::Custom);
  spring.setCustomType(springProgress);
  openAnimation_.setEasingCurve(spring);
  openAnimation_.setStartValue(0.0);
  openAnimation_.setEndValue(1.0);
  connect(&openAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    open01_ = closing_ ? 1 - v.toReal() : v.toReal();
    update();
  });
  connect(&openAnimation_, &QVariantAnimation::finished, this, [this] {
    backdrop_ = {};
    if (closing_) closeViewer();
    else {
      open01_ = 1;
      preview_ = fit_.isNull() ? preview_ : QImage();
      update();
    }
  });
  chromeFade_.setDuration(300);
  connect(&chromeFade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    chrome01_ = v.toReal();
    for (QWidget *widget : {topBar_, static_cast<QWidget *>(previousButton_), static_cast<QWidget *>(nextButton_)}) {
      if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect())) effect->setOpacity(chrome01_);
    }
  });
  connect(&chromeFade_, &QVariantAnimation::finished, this, [this] {
    for (QWidget *widget : {topBar_, static_cast<QWidget *>(previousButton_), static_cast<QWidget *>(nextButton_)}) {
      widget->setGraphicsEffect(nullptr);
      if (!chromeShown_) widget->hide();
    }
    update();
  });
  pointerPoll_.setInterval(150);
  connect(&pointerPoll_, &QTimer::timeout, this, [this] {
    const QPoint now = QCursor::pos();
    if (now != lastPointer_ && rect().contains(mapFromGlobal(now))) wakeChrome();
    lastPointer_ = now;
  });
  animationTimer_.setSingleShot(true);
  animationTimer_.setTimerType(Qt::PreciseTimer);
  connect(&animationTimer_, &QTimer::timeout, this, &Viewer::animationStep);
  qApp->installEventFilter(this);
}
Viewer::~Viewer() {
  generation_++;
  decoders_.clear();
  decoders_.waitForDone();  // A decode in flight may still post to this object.
}
QJsonObject Viewer::videoChromeState() const {
  const auto global = [](const QWidget *w) {
    const QPoint p = w->mapToGlobal(QPoint(0, 0));
    return QJsonObject{{QStringLiteral("x"), p.x()}, {QStringLiteral("y"), p.y()}, {QStringLiteral("w"), w->width()}, {QStringLiteral("h"), w->height()}};
  };
  QJsonObject out{{QStringLiteral("barVisible"), videoBar_->isVisible()}, {QStringLiteral("bar"), global(videoBar_)}, {QStringLiteral("seek"), global(seek_)}, {QStringLiteral("volumeBar"), global(volume_)}, {QStringLiteral("mute"), global(muteButton_)}, {QStringLiteral("play"), global(playButton_)}, {QStringLiteral("fullscreen"), global(fullscreenButton_)}, {QStringLiteral("muted"), savedMuted_}, {QStringLiteral("volume"), double(savedVolume_)}, {QStringLiteral("windowFullScreen"), window()->isFullScreen()}};
  if (video_) out[QStringLiteral("video")] = global(video_);
  return out;
}
bool Viewer::eventFilter(QObject *object, QEvent *event) {
  // Space on a focused chrome button does not press it; it acts on the viewer.
  if (open_ && event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space && object->isWidgetType() && object != this && isAncestorOf(static_cast<QWidget *>(object))) {
    setFocus();
    keyPressEvent(static_cast<QKeyEvent *>(event));
    return true;
  }
  // Any pointer motion over the viewer or its video surface brings the controls back.
  if (open_ && event->type() == QEvent::MouseMove && object->isWidgetType() && (object == this || isAncestorOf(static_cast<QWidget *>(object)))) wakeChrome();
  return QWidget::eventFilter(object, event);
}
// Hidden at once, and remembered as hidden, so the next open fades it in again.
void Viewer::hideChrome() {
  chromeFade_.stop();
  chromeTimer_.stop();
  chromeShown_ = false;
  chrome01_ = 0;
  for (QWidget *widget : {topBar_, static_cast<QWidget *>(previousButton_), static_cast<QWidget *>(nextButton_)}) {
    widget->setGraphicsEffect(nullptr);
    widget->hide();
  }
  videoBar_->hide();
}
void Viewer::wakeChrome() {
  setChromeVisible(true);
  chromeTimer_.start();
}
void Viewer::setChromeVisible(bool visible) {
  // The file name and actions stay reachable by keyboard: they hide only while the pointer rests.
  const bool hasFocusInside = topBar_->isAncestorOf(QApplication::focusWidget()) || videoBar_->isAncestorOf(QApplication::focusWidget());
  if (!visible && hasFocusInside && focusVisible()) return;
  if (visible == chromeShown_) return;
  chromeShown_ = visible;
  const QVector<QWidget *> widgets{topBar_, previousButton_, nextButton_};
  previousButton_->setVisible(index_ > 0);
  nextButton_->setVisible(index_ + 1 < total_);
  topBar_->show();
  // A native surface takes no opacity effect: the bar over the video appears and goes at once.
  videoBar_->setVisible(item_.isVideo && visible);
  if (videoBar_->isVisible()) videoBar_->raise();
  const int duration = Theme::motion(visible ? 150 : 300);
  if (!duration) {
    for (QWidget *widget : widgets)
      if (!visible) widget->hide();
    return;
  }
  for (QWidget *widget : widgets) {
    auto *effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(chrome01_);
    widget->setGraphicsEffect(effect);
  }
  chromeFade_.stop();
  chromeFade_.setDuration(duration);
  chromeFade_.setStartValue(chrome01_);
  chromeFade_.setEndValue(visible ? 1.0 : 0.0);
  chromeFade_.start();
}
// Inside the bottom of the picture; a very narrow video still gets a usable bar.
void Viewer::placeVideoBar() {
  const QRect frame = video_ ? video_->geometry().intersected(mediaArea().toAlignedRect()) : mediaArea().toAlignedRect();
  const int barWidth = qMax(qMin(frame.width() - 24, width() - 48), qMin(440, width() - 48));
  videoBar_->setGeometry(QRect(mapToGlobal(QPoint(frame.center().x() - barWidth / 2, frame.bottom() + 1 - 12 - 40)), QSize(barWidth, 40)));
  videoBar_->raise();
}
void Viewer::layoutChrome() {
  topBar_->setGeometry(0, 0, width(), 72);
  previousButton_->move(16, (height() - previousButton_->height()) / 2);
  nextButton_->move(width() - nextButton_->width() - 16, (height() - nextButton_->height()) / 2);
  placeVideoBar();
  failureBox_->adjustSize();
  failureBox_->move((width() - failureBox_->width()) / 2, (height() - failureBox_->height()) / 2);
}
QRectF Viewer::mediaArea() const { return QRectF(rect()).adjusted(kBounds, kBounds, -kBounds, -kBounds); }
QRectF Viewer::fitRect(QSizeF content) const {
  const QRectF area = mediaArea();
  if (content.isEmpty()) return area;
  QSizeF scaled = content;
  if (scaled.width() > area.width() || scaled.height() > area.height()) scaled.scale(area.size(), Qt::KeepAspectRatio);
  return QRectF(area.center().x() - scaled.width() / 2, area.center().y() - scaled.height() / 2, scaled.width(), scaled.height());
}
QRectF Viewer::imageRect() const {
  // CSS pixels: an image is never enlarged past its natural size, and one image pixel is one CSS pixel.
  QSizeF content(originalSize_);
  if (content.isEmpty() && !preview_.isNull()) content = item_.width > 0 ? QSizeF(item_.width, item_.height) : QSizeF(preview_.size());
  const QRectF fit = fitRect(content);
  const QSizeF scaled = fit.size() * zoom_;
  return QRectF(fit.center().x() - scaled.width() / 2 + pan_.x(), fit.center().y() - scaled.height() / 2 + pan_.y(), scaled.width(), scaled.height());
}
QSize Viewer::decodeBox() const { return (size() * devicePixelRatioF()).grownBy(QMargins()).boundedTo(QSize(16384, 16384)); }
void Viewer::setPosition(int index, int total) {
  index_ = index;
  total_ = total;
  previousButton_->setVisible(chromeShown_ && index > 0);
  nextButton_->setVisible(chromeShown_ && index + 1 < total);
}
void Viewer::setFavorite(bool favorite) {
  item_.favorite = favorite;
  favoriteButton_->setIcon(favorite ? "heart-filled" : "heart");
  favoriteButton_->setTint(favorite ? Theme::red500 : QColor());
  favoriteButton_->setLabel(favorite ? t("media.unfavorite") : t("media.favorite"));
}
void Viewer::retranslate() {
  setFavorite(item_.favorite);
  copyButton_->setLabel(t("media.copy"));
  deleteButton_->setLabel(t("contextMenu.delete"));
  closeButton_->setLabel(t("common.close"));
  previousButton_->setLabel(t("media.previous"));
  nextButton_->setLabel(t("media.next"));
  playButton_->setLabel(player_ && player_->playbackState() == QMediaPlayer::PlayingState ? t("video.pause") : t("video.play"));
  backButton_->setLabel(t("video.back10"));
  forwardButton_->setLabel(t("video.forward10"));
  seek_->setAccessibleName(t("video.seek"));
  muteButton_->setLabel(savedMuted_ ? t("video.unmute") : t("video.mute"));
  volume_->setAccessibleName(t("video.volume"));
  fullscreenButton_->setLabel(t("viewer.fullscreen"));
  failureText_->setText(t("media.displayFailed"));
  retryButton_->setText(t("common.retry"));
  failureRevealButton_->setText(t("media.revealOriginal"));
}
void Viewer::showItem(const MediaItem &item, const QRect &origin, const QImage &preview) {
  const bool wasOpen = open_;
  // The picture being left stays decoded for a moment: stepping back to it shows it at once.
  if (wasOpen && !fit_.isNull() && !item_.isVideo && item_.path() != item.path()) keepDecoded(item_.path(), fit_);
  animationTimer_.stop();
  animation_.reset();
  item_ = item;
  open_ = true;
  closing_ = false;
  ready_ = false;
  failed_ = false;
  zoom_ = 1;
  pan_ = {};
  full_ = {};
  fit_ = {};
  originalSize_ = item.width > 0 && item.height > 0 ? QSize(item.width, item.height) : QSize();
  preview_ = preview;
  hideFailure();
  const int generation = ++generation_;
  Q_UNUSED(generation);
  setFavorite(item.favorite);
  titleLabel_->setText(QFontMetrics(titleLabel_->font()).elidedText(item.label(), Qt::ElideMiddle, qMax(120, width() - 340)));
  const QLocale locale(I18n::language() == QLatin1String("ru") ? QLocale::Russian : QLocale::English);
  // Position in the system number format, then a medium date and a short time.
  QStringList parts;
  if (index_ >= 0 && total_ > 0) parts << QStringLiteral("%1 / %2").arg(QLocale::system().toString(index_ + 1), QLocale::system().toString(total_));
  if (const double when = item.displayDate(); when > 0) {
    const bool ru = I18n::language() == QLatin1String("ru");
    const QDateTime moment = QDateTime::fromSecsSinceEpoch(qint64(when));
    // Medium date for ru: the abbreviated genitive month.
    static const char *ruMonths[] = {"янв.", "февр.", "мар.", "апр.", "мая", "июня", "июля", "авг.", "сент.", "окт.", "нояб.", "дек."};
    parts << (ru ? QStringLiteral("%1 %2 %3 г., %4").arg(moment.date().day()).arg(QString::fromUtf8(ruMonths[moment.date().month() - 1])).arg(moment.date().year()).arg(moment.toString(QStringLiteral("HH:mm"))) : locale.toString(moment, QStringLiteral("MMM d, yyyy, h:mm AP")));
  }
  subtitleLabel_->setText(parts.join(QStringLiteral(" · ")));
  setGeometry(parentWidget()->rect());
  layoutChrome();
  show();
  raise();
  setFocus();
  wakeChrome();
  if (!wasOpen) {
    origin_ = origin;
    const int duration = Theme::motion(170);
    if (duration && !suspended_) {
      open01_ = 0;
      openAnimation_.stop();
      openAnimation_.setDirection(QAbstractAnimation::Forward);
      openAnimation_.setDuration(duration);
      openAnimation_.start();
    } else {
      open01_ = 1;
      backdrop_ = {};
    }
  }
  videoBar_->setVisible(item.isVideo && chromeShown_);
  if (item.isVideo) {
    lastPointer_ = QCursor::pos();
    pointerPoll_.start();
  } else {
    pointerPoll_.stop();
  }
  if (item.isVideo) {
    startVideo();
    update();
    return;
  }
  releasePlayer();
  update();
  startDecode();
}
void Viewer::startDecode() {
  const QString path = item_.path();
  const int generation = generation_.load();
  const QString ext = item_.extension();
  if (ext == QLatin1String("gif") || ext == QLatin1String("webp") || ext == QLatin1String("apng")) {
    startAnimation();
    if (animation_) return;
  }
  auto cached = prefetched_.constFind(path);
  if (cached != prefetched_.cend()) {
    const QImage fit = *cached;
    prefetched_.erase(prefetched_.find(path));
    prefetchOrder_.removeAll(path);
    delivered(generation, fit, originalSize_.isValid() ? originalSize_ : fit.size());
    return;
  }
  const QSize box = decodeBox();
  decoders_.clear();  // Requests not yet started are stale now.
  decoders_.start([this, path, box, generation] {
    if (generation != generation_.load()) return;
    QSize original;
    QImage fit = decodeForDisplay(path, box, &original);
    QMetaObject::invokeMethod(this, [this, generation, original, fit = std::move(fit)] { delivered(generation, fit, original); }, Qt::QueuedConnection);
  });
}
// Both neighbours and the picture just left: four display-sized decodes at most.
void Viewer::keepDecoded(const QString &path, const QImage &fit) {
  prefetchOrder_.removeAll(path);
  prefetched_.insert(path, fit);
  prefetchOrder_.push_back(path);
  while (prefetchOrder_.size() > 4) prefetched_.remove(prefetchOrder_.takeFirst());
}
void Viewer::prefetch(const MediaItem &item) {
  if (item.isVideo || prefetched_.contains(item.path()) || suspended_) return;
  if (item.extension() == QLatin1String("gif")) return;
  const QString path = item.path();
  const QSize box = decodeBox();
  const int generation = generation_.load();
  decoders_.start([this, path, box, generation] {
    if (generation != generation_.load()) return;
    QImage fit = decodeForDisplay(path, box);
    QMetaObject::invokeMethod(this, [this, path, fit = std::move(fit)] {
      if (fit.isNull() || !open_ || path == item_.path()) return;
      keepDecoded(path, fit);
    }, Qt::QueuedConnection);
  }, -1);
}
void Viewer::startAnimation() {
  std::shared_ptr<QBuffer> plain;
  std::shared_ptr<QImageReader> reader;
  if (SecureMedia::isSealed(item_.path())) {
    plain = std::make_shared<QBuffer>();
    plain->setData(SecureMedia::read(item_.path()));
    plain->open(QIODevice::ReadOnly);
    reader = std::make_shared<QImageReader>(plain.get());
  } else {
    reader = std::make_shared<QImageReader>(item_.path());
  }
  reader->setAutoTransform(true);
  if (!reader->supportsAnimation() || reader->imageCount() == 1) return;
  animation_ = std::make_unique<Animation>();
  animation_->plain = plain;
  animation_->reader = reader;
  animation_->box = decodeBox();
  animation_->generation = generation_.load();
  animationFrames_ = 0;
  if (!suspended_) requestAnimationFrame();
}
void Viewer::requestAnimationFrame() {
  if (!animation_ || animation_->pending) return;
  animation_->pending = true;
  auto reader = animation_->reader;
  auto plain = animation_->plain;
  const QSize box = animation_->box;
  const int generation = animation_->generation;
  decoders_.start([this, reader, plain, box, generation] {
    if (generation != generation_.load()) return;
    QImage frame = reader->read();
    if (frame.isNull()) {
      // The last frame was reached or the file ended early: rewind for the next loop.
      reader->jumpToImage(0);
      if (reader->error() != QImageReader::UnknownError || !reader->canRead()) {
        if (plain) {
          plain->seek(0);
          reader->setDevice(plain.get());
        } else {
          const QString file = reader->fileName();
          reader->setFileName(file);
        }
      }
      frame = reader->read();
    }
    const int delay = qMax(20, reader->nextImageDelay());
    const QSize original = reader->size();
    if (!frame.isNull()) {
      if (frame.width() > box.width() || frame.height() > box.height()) frame = frame.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      frame = frame.convertToFormat(frame.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    }
    QMetaObject::invokeMethod(this, [this, generation, delay, original, frame = std::move(frame)] {
      if (!animation_ || generation != animation_->generation) return;
      animation_->pending = false;
      if (frame.isNull()) {
        if (!ready_) showFailure();
        return;
      }
      animation_->ready = frame;
      animation_->readyDelay = delay;
      if (!ready_) {
        if (original.isValid()) originalSize_ = original;
        animationStep();
        ready_ = true;
        emit becameReady();
      }
    }, Qt::QueuedConnection);
  });
}
void Viewer::animationStep() {
  if (!animation_ || suspended_ || !isVisible()) return;
  if (animation_->ready.isNull()) {
    // The decoder is behind: try again shortly instead of skipping frames.
    animationTimer_.start(8);
    return;
  }
  fit_ = animation_->ready;
  animation_->ready = {};
  animationFrames_++;
  update();
  animationTimer_.start(animation_->readyDelay);
  requestAnimationFrame();
}
void Viewer::setSuspended(bool suspended) {
  // The controls are their own window: they must not stay above the lock screen or the privacy shield.
  if (suspended) videoBar_->hide();
  else if (open_ && item_.isVideo && chromeShown_) {
    placeVideoBar();
    videoBar_->show();
  }
  if (suspended == suspended_) return;
  suspended_ = suspended;
  if (suspended) {
    animationTimer_.stop();
    if (player_ && player_->playbackState() == QMediaPlayer::PlayingState) {
      resumeVideo_ = true;
      player_->pause();
    }
  } else {
    if (animation_) {
      if (animation_->ready.isNull()) requestAnimationFrame();
      animationTimer_.start(animation_->readyDelay);
    }
    if (player_ && resumeVideo_) player_->play();
    resumeVideo_ = false;
  }
}
void Viewer::delivered(int generation, QImage fit, QSize original) {
  if (generation != generation_.load()) return;
  if (fit.isNull()) {
    showFailure();
    return;
  }
  if (original.isValid()) originalSize_ = original;
  else if (!originalSize_.isValid()) originalSize_ = fit.size();
  fit_ = std::move(fit);
  if (openAnimation_.state() != QAbstractAnimation::Running) preview_ = {};
  ready_ = true;
  update();
  emit becameReady();
}
void Viewer::ensureFull() {
  if (!full_.isNull() || fullPending_ == generation_.load() || item_.isVideo || animation_) return;
  const int generation = generation_.load();
  fullPending_ = generation;
  const QString path = item_.path();
  decoders_.start([this, path, generation] {
    if (generation != generation_.load()) return;
    QImage full = decodeOriginal(path);
    QMetaObject::invokeMethod(this, [this, generation, full = std::move(full)] {
      if (generation != generation_.load()) return;
      full_ = full;
      update();
    }, Qt::QueuedConnection);
  });
}
void Viewer::startVideo() {
  if (!player_) {
    player_ = new QMediaPlayer(this);
    audio_ = new QAudioOutput(this);
    video_ = new QVideoWidget(this);
    video_->setMouseTracking(true);
    video_->installEventFilter(this);
    audio_->setMuted(savedMuted_);
    audio_->setVolume(savedVolume_);
    player_->setAudioOutput(audio_);
    player_->setVideoOutput(video_);
    if (gQaTraceOn) {
      const QAudioDevice device = QMediaDevices::defaultAudioOutput();
      qaTrace("audio-device", QStringLiteral("%1 rate=%2").arg(device.description()).arg(device.preferredFormat().sampleRate()));
      connect(player_, &QMediaPlayer::mediaStatusChanged, this, [](QMediaPlayer::MediaStatus s) { qaTrace("media-status", QString::number(int(s))); });
      connect(player_, &QMediaPlayer::playbackStateChanged, this, [](QMediaPlayer::PlaybackState s) { qaTrace("playback-state", QString::number(int(s))); });
      connect(player_, &QMediaPlayer::playbackRateChanged, this, [](qreal r) { qaTrace("playback-rate", QString::number(r)); });
      connect(player_, &QMediaPlayer::bufferProgressChanged, this, [](float p) { qaTrace("buffer", QString::number(p)); });
    }
    connect(player_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error e, const QString &text) {
      qaTrace("media-error", QStringLiteral("%1 %2").arg(int(e)).arg(text));
      if (e != QMediaPlayer::NoError) showFailure();
    });
    connect(player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus s) {
      if (s == QMediaPlayer::InvalidMedia) showFailure();
      if (s == QMediaPlayer::EndOfMedia) {
        playButton_->setIcon("rotate-clockwise");
        playButton_->setLabel(t("video.play"));
      }
    });
    connect(player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
      const bool playing = state == QMediaPlayer::PlayingState;
      playButton_->setIcon(playing ? "player-pause-filled" : "player-play-filled");
      playButton_->setLabel(playing ? t("video.pause") : t("video.play"));
    });
    connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
      const qint64 duration = player_->duration();
      if (duration > 0) seek_->setValue(qreal(position) / duration);
      timeLabel_->setText(clock(position));
      durationLabel_->setText(clock(duration));
    });
    connect(video_->videoSink(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
      if (gQaTraceOn) qaTrace("frame", QStringLiteral("pts=%1 pos=%2").arg(frame.startTime()).arg(player_ ? player_->position() : -1));
      if (!item_.isVideo || !frame.isValid()) return;  // Queued before the switch to a still image, or nothing to show.
      videoFrames_++;
      if (!ready_) {
        ready_ = true;
        if (frame.isValid() && frame.size().isValid()) originalSize_ = frame.size();
        applyVideoGeometry();
        emit becameReady();
      }
    });
  }
  videoFrames_ = 0;
  seek_->setValue(0);
  applyVideoGeometry();
  video_->show();
  video_->setFocusProxy(this);
  if (SecureMedia::isSealed(item_.path())) {
    // The player reads the decrypted stream; the original name tells it the container.
    std::unique_ptr<QIODevice> device = SecureMedia::open(item_.path());
    if (!device) {
      showFailure();
      return;
    }
    // The previous stream is released only after the player has let go of it.
    player_->setSourceDevice(device.get(), QUrl::fromLocalFile(QStringLiteral("/") + item_.label()));
    sealedVideo_ = std::move(device);
  } else {
    player_->setSource(QUrl::fromLocalFile(item_.path()));
  }
  if (!suspended_) player_->play();
  else resumeVideo_ = true;
}
void Viewer::applyVideoGeometry() {
  if (!video_) return;
  const QRectF fit = fitRect(QSizeF(originalSize_));
  // Zoom enlarges and pans the widget inside the media bounds; corners rounded like the images.
  const QSizeF scaled = fit.size() * zoom_;
  const QRectF target(fit.center().x() - scaled.width() / 2 + pan_.x(), fit.center().y() - scaled.height() / 2 + pan_.y(), scaled.width(), scaled.height());
  video_->setGeometry(target.toRect());
  QPainterPath round;
  round.addRoundedRect(QRectF(QPointF(0, 0), target.size()), Theme::kRadius3xl, Theme::kRadius3xl);
  const QRegion rounded(round.toFillPolygon().toPolygon());
  video_->setMask(rounded);
  // The surface is a QWindow parented to the top-level window, so a widget mask does not reach it.
  if (QWindow *top = window()->windowHandle()) {
    for (QWindow *surface : top->findChildren<QWindow *>()) {
      if (!surface->inherits("QVideoWindow")) continue;
      surface->setMask(rounded);
      Platform::roundSurface(surface, Theme::kRadius3xl);
    }
  }
  video_->lower();
  placeVideoBar();
}
void Viewer::releasePlayer() {
  if (!player_) return;
  player_->stop();
  player_->setSource({});
  sealedVideo_.reset();
  // The decoder and video pipeline hold hundreds of MiB; keep them only while a video is open.
  if (envFlag("ROOM237_RELEASE_VIDEO", true)) {
    delete video_;
    delete player_;
    delete audio_;
    video_ = nullptr;
    player_ = nullptr;
    audio_ = nullptr;
  } else {
    video_->hide();
  }
  videoBar_->hide();
}
void Viewer::showFailure() {
  failed_ = true;
  if (video_) video_->hide();
  failureBox_->show();
  failureBox_->raise();
  layoutChrome();
  update();
}
void Viewer::hideFailure() {
  failed_ = false;
  failureBox_->hide();
}
void Viewer::beginClose() {
  if (!open_ || closing_) return;
  emit closing();
  const int duration = Theme::motion(170);
  if (!duration || item_.isVideo || suspended_) return closeViewer();
  closing_ = true;
  animationTimer_.stop();
  hideChrome();
  if (backdrop_.isNull()) backdrop_ = frost_;
  openAnimation_.stop();
  openAnimation_.setDuration(duration);
  openAnimation_.start();
}
void Viewer::closeViewer() {
  if (!open_) return;
  pointerPoll_.stop();
  open_ = false;
  closing_ = false;
  generation_++;
  openAnimation_.stop();
  animationTimer_.stop();
  animation_.reset();
  releasePlayer();
  full_ = {};
  fit_ = {};
  preview_ = {};
  backdrop_ = {};
  frost_ = {};
  prefetched_.clear();
  prefetchOrder_.clear();
  hideFailure();
  hideChrome();
  hide();
  emit closed();
}
void Viewer::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  // The blurred copy of the app is taken once when the viewer opens.
  // The spring overshoots: geometry follows the raw progress, opacity stays within 0..1.
  const bool flying = openAnimation_.state() == QAbstractAnimation::Running;
  const qreal open = flying ? open01_ : 1, fade = qBound<qreal>(0, open, 1);
  if (flying && !backdrop_.isNull()) {
    painter.drawPixmap(0, 0, backdrop_);
    painter.setOpacity(fade);
  }
  if (!frost_.isNull()) painter.drawPixmap(0, 0, frost_);
  else painter.fillRect(rect(), Theme::background);
  painter.fillRect(rect(), QColor(0, 0, 0, 178));
  painter.setOpacity(1);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.setRenderHint(QPainter::Antialiasing);
  if (item_.isVideo || failed_) return;
  const QImage &base = fit_.isNull() ? preview_ : fit_;
  if (base.isNull()) return;
  QRectF target = imageRect(), box = target;
  if (flying && !origin_.isNull()) {
    // From the tile where it sits in the list: the frame grows out of the tile, and the picture, which the tile crops
    // to fill it, opens up to the whole frame without stretching.
    const QRectF tile(origin_);
    const qreal cover = qMax(tile.width() / target.width(), tile.height() / target.height());
    QRectF from(QPointF(0, 0), target.size() * cover);
    from.moveCenter(tile.center());
    const auto mix = [open](const QRectF &a, const QRectF &b) {
      return QRectF(a.x() + (b.x() - a.x()) * open, a.y() + (b.y() - a.y()) * open, a.width() + (b.width() - a.width()) * open, a.height() + (b.height() - a.height()) * open);
    };
    box = mix(tile, target);
    target = mix(from, target);
  } else if (flying) {
    const qreal scale = 0.9 + 0.1 * open;
    const QPointF center = target.center();
    target.setSize(target.size() * scale);
    target.moveCenter(center);
    box = target;
    painter.setOpacity(fade);
  }
  const QRectF frame = zoom_ > 1.001 ? mediaArea() : box;
  const qreal radius = flying && !origin_.isNull() ? qMax<qreal>(0, originRadius_ + (Theme::kRadius3xl - originRadius_) * open) : Theme::kRadius3xl;
  QPainterPath clip;
  clip.addRoundedRect(frame, radius, radius);
  painter.save();
  painter.setClipPath(clip);
  painter.fillRect(frame, QColor(0, 0, 0, 77));
  if (zoom_ <= 1.001) {
    QRectF where = target;
    // A list preview is the middle of the picture cropped to its tile; it covers that part only, unstretched.
    if (fit_.isNull() && !target.isEmpty()) {
      const qreal picture = target.width() / target.height(), crop = qreal(base.width()) / base.height();
      if (qAbs(picture - crop) > 0.01) {
        where.setSize(crop > picture ? QSizeF(target.width(), target.width() / crop) : QSizeF(target.height() * crop, target.height()));
        where.moveCenter(target.center());
      }
    }
    painter.drawImage(where, base);
  } else {
    const QRectF visible = target.intersected(frame);
    const QImage &image = full_.isNull() ? base : full_;  // The fitted image stays on screen until full pixels arrive.
    const double sx = image.width() / target.width(), sy = image.height() / target.height();
    painter.drawImage(visible, image, QRectF((visible.x() - target.x()) * sx, (visible.y() - target.y()) * sy, visible.width() * sx, visible.height() * sy));
  }
  painter.restore();
  painter.setOpacity(1);
  if (zoom_ > 1.001 && !closing_) {
    // Minimap inside the media, with the visible part outlined.
    const qreal mapHeight = kMinimap * target.height() / qMax<qreal>(1, target.width());
    const QRectF map(frame.left() + 16, frame.top() + 16, kMinimap, mapHeight);
    QPainterPath shape;
    shape.addRoundedRect(map, Theme::kRadiusLg, Theme::kRadiusLg);
    painter.save();
    painter.setClipPath(shape);
    painter.fillRect(map, QColor(0, 0, 0, 102));
    painter.drawImage(map, base);
    painter.restore();
    painter.setPen(QPen(QColor(255, 255, 255, 51), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);
    const QRectF visible = target.intersected(frame);
    const QRectF window(map.x() + (visible.x() - target.x()) / target.width() * map.width(), map.y() + (visible.y() - target.y()) / target.height() * map.height(), visible.width() / target.width() * map.width(), visible.height() / target.height() * map.height());
    painter.setPen(QPen(QColor(255, 255, 255, 204), 1));
    painter.setBrush(QColor(255, 255, 255, 51));
    painter.drawRoundedRect(window, Theme::kRadiusLg, Theme::kRadiusLg);
  }
}
void Viewer::clampPan() {
  if (zoom_ <= 1.001) {
    pan_ = {};
    return;
  }
  const QRectF area = mediaArea();
  const QSizeF content = fitRect(QSizeF(originalSize_)).size() * zoom_;
  const qreal mx = qMax<qreal>(0, (content.width() - area.width()) / 2), my = qMax<qreal>(0, (content.height() - area.height()) / 2);
  pan_.setX(qBound(-mx, pan_.x(), mx));
  pan_.setY(qBound(-my, pan_.y(), my));
}
void Viewer::zoomAt(double target, QPointF focal) {
  target = qBound(1.0, target, kMaxZoom);
  const double factor = zoom_ > 0 ? target / zoom_ : 1;
  const QPointF center = mediaArea().center();
  // Keep the point under the pointer in place.
  pan_ = (pan_ - (focal - center)) * factor + (focal - center);
  zoom_ = target;
  clampPan();
  if (zoom_ > 1.001) ensureFull();
  if (item_.isVideo) applyVideoGeometry();
  update();
}
void Viewer::setZoom(double zoom) { zoomAt(zoom, mediaArea().center()); }
void Viewer::keyPressEvent(QKeyEvent *event) {
  const bool command = event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
  switch (event->key()) {
    case Qt::Key_Equal:
    case Qt::Key_Plus:
    case Qt::Key_Minus:
      if (command) zoomAt(zoom_ * (event->key() == Qt::Key_Minus ? 1 / (1 + kWheelStep) : 1 + kWheelStep), mediaArea().center());
      break;
    case Qt::Key_0:
      if (command) {
        zoom_ = 1;
        pan_ = {};
        if (item_.isVideo) applyVideoGeometry();
        update();
      }
      break;
    // A held arrow keeps stepping with the key repeat.
    case Qt::Key_Left:
    case Qt::Key_Up:
      if (!command) emit navigate(-1);
      break;
    case Qt::Key_Right:
    case Qt::Key_Down:
      if (!command) emit navigate(1);
      break;
    case Qt::Key_Escape:
      if (!event->isAutoRepeat()) beginClose();
      break;
    case Qt::Key_Space:
      if (event->isAutoRepeat()) break;
      if (item_.isVideo && player_) playButton_->click();
      else beginClose();
      break;
    default: QWidget::keyPressEvent(event);
  }
}
void Viewer::wheelEvent(QWheelEvent *event) {
  const bool command = event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
  const QPoint pixel = event->pixelDelta();
  const QPointF focal = event->position();
  if (command) {
    const qreal delta = pixel.isNull() ? event->angleDelta().y() / 8.0 : pixel.y();
    zoomAt(zoom_ * std::pow(1.01, delta), focal);
  } else if (zoom_ > 1.001) {
    const QPointF delta = pixel.isNull() ? QPointF(event->angleDelta()) / 2 : QPointF(pixel);
    pan_ += delta;
    clampPan();
    if (item_.isVideo) applyVideoGeometry();
    update();
  }
  event->accept();
}
bool Viewer::event(QEvent *event) {
  if (event->type() == QEvent::NativeGesture) {
    auto *gesture = static_cast<QNativeGestureEvent *>(event);
    if (gesture->gestureType() == Qt::ZoomNativeGesture) {
      zoomAt(zoom_ * (1 + gesture->value()), gesture->position());
      return true;
    }
    if (gesture->gestureType() == Qt::SmartZoomNativeGesture) {
      zoomAt(zoom_ > 1.001 ? 1 : 2.5, gesture->position());
      return true;
    }
  }
  return QWidget::event(event);
}
void Viewer::mousePressEvent(QMouseEvent *event) {
  dragging_ = true;
  moved_ = false;
  lastPos_ = pressPos_ = event->position();
  if (zoom_ > 1.001) setCursor(Qt::ClosedHandCursor);
}
void Viewer::mouseMoveEvent(QMouseEvent *event) {
  if (!dragging_) return;
  if ((event->position() - pressPos_).manhattanLength() > 4) moved_ = true;
  if (zoom_ <= 1.001) return;
  pan_ += event->position() - lastPos_;
  lastPos_ = event->position();
  clampPan();
  if (item_.isVideo) applyVideoGeometry();
  update();
}
void Viewer::mouseReleaseEvent(QMouseEvent *event) {
  const bool click = dragging_ && !moved_;
  dragging_ = false;
  unsetCursor();
  if (!click || zoom_ > 1.001 || item_.isVideo || failed_) return;
  // A click beside the picture closes, a click on it does not.
  if (!imageRect().contains(event->position())) beginClose();
}
void Viewer::mouseDoubleClickEvent(QMouseEvent *event) {
  if (item_.isVideo) return;
  zoomAt(zoom_ > 1.001 ? 1 : 2.5, event->position());
}
void Viewer::resizeEvent(QResizeEvent *) {
  layoutChrome();
  clampPan();
  applyVideoGeometry();
}
