#pragma once
// Full-window screens: choose folder, confirm open, lock, debugger, indexing, startup.
#include "widgets.h"
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMovie>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
// A Lottie animation pre-rendered from its JSON (qt/tools) and played from a resource.
class LottieView : public QWidget {
public:
  LottieView(const QString &name, int side, QWidget *parent = nullptr);
protected:
  void paintEvent(QPaintEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;
private:
  QMovie movie_;
};
class HeroScreen : public QWidget {
  Q_OBJECT
public:
  // A centred column: animation, title, muted text, buttons.
  HeroScreen(QWidget *parent, const QString &lottie, bool overlay = false);
  void setTexts(const QString &title, const QString &subtitle);
  void addButton(TextButton *button);
  void addVerticalButton(TextButton *button);
  void setSubtitleStyle(int pixelSize, const char *color);
  void setFrost(const QPixmap &frost) { frost_ = frost; update(); }
protected:
  void paintEvent(QPaintEvent *) override;
private:
  void fitTexts();
  int subtitleLimit_ = 352, subtitleLine_ = 120;  // Percent of the natural line: text-sm 20px, text-base 24px.
  QString subtitleRaw_;
  QLabel *title_, *subtitle_;
  QWidget *buttons_;
  QVBoxLayout *column_;
  QPixmap frost_;
  bool overlay_;
};
// A spinner or an error icon, the phase, an optional message and retry.
class StartupScreen : public QWidget {
  Q_OBJECT
public:
  explicit StartupScreen(QWidget *parent);
  void setBusy(const QString &text);
  void setError(const QString &text, const QString &message);
signals:
  void retry();
protected:
  void paintEvent(QPaintEvent *) override;
private:
  Spinner *spinner_;
  QLabel *icon_, *title_, *message_;
  TextButton *retry_;
};
// Opaque picture over the app while it is unfocused with privacy on; never live content.
class PrivacyShield : public QWidget {
  Q_OBJECT
public:
  explicit PrivacyShield(QWidget *parent);
  void showFrom(QWidget *source);
protected:
  void paintEvent(QPaintEvent *) override;
private:
  QPixmap frost_;
};
// Over the whole window while a library is being indexed or encrypted: animation, title, steps with bars. It takes
// every click, scroll and drop, and goes away only when the work is done. Indexing shows the worker counts, which
// change the speed at once.
class PreloadOverlay : public QWidget {
  Q_OBJECT
public:
  explicit PreloadOverlay(QWidget *parent);
  void setStages(int thumbsDone, int thumbsTotal, int metaDone, int metaTotal);
  void finish();
  // Encryption or its removal: one step of files.
  void setVault(const QString &title, int done, int total);
  void finishVault();
  void setWorkers(int thumbnails, int metadata, int encryption);
  void retranslate();
#ifdef ROOM237_QA
  bool qaVault() const { return vault_; }
  void qaSetWorkers(int thumbnails, int metadata, int encryption) {
    thumbWorkers_->setValue(thumbnails, true);
    metaWorkers_->setValue(metadata, true);
    vaultWorkers_->setValue(encryption, true);
  }
  bool qaFrosted() const { return !frost_.isNull(); }
#endif
signals:
  void workersChanged(int thumbnails, int metadata, int encryption);
  void blockingChanged(bool blocking);
protected:
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;
  void mousePressEvent(QMouseEvent *event) override { event->accept(); }
  void mouseReleaseEvent(QMouseEvent *event) override { event->accept(); }
  void mouseDoubleClickEvent(QMouseEvent *event) override { event->accept(); }
  void wheelEvent(QWheelEvent *event) override { event->accept(); }
private:
  void appear();
  void refreshFrost();
  void disappear();
  void place();
  void paintStep(QPainter &p, int y, const QString &name, int done, int total, bool last);
  int thumbsDone_ = 0, thumbsTotal_ = 0, metaDone_ = 0, metaTotal_ = 0, vaultDone_ = 0, vaultTotal_ = 0;
  bool preloading_ = false, vault_ = false;
  QString vaultTitle_;
  QTimer showDelay_, idle_, frostTimer_;
  QPixmap frost_;
  QVariantAnimation fade_, spin_;
  qreal opacity_ = 0;
  LottieView *lottie_;
  NumberSlider *thumbWorkers_, *metaWorkers_, *vaultWorkers_;
};
// The latest log lines in a small corner panel.
class LoggerPanel : public QWidget {
public:
  explicit LoggerPanel(QWidget *parent);
protected:
  void paintEvent(QPaintEvent *) override;
private:
  QTimer refresh_;
};
