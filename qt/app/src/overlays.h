#pragma once
// Full-window screens: choose folder, confirm open, lock, debugger, indexing, startup.
#include "widgets.h"
#include <QLabel>
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
// Over the gallery without capturing the pointer: animation, title, step list with bars.
class PreloadOverlay : public QWidget {
  Q_OBJECT
public:
  explicit PreloadOverlay(QWidget *parent);
  void setStages(int thumbsDone, int thumbsTotal, int metaDone, int metaTotal);
  void finish();
protected:
  void paintEvent(QPaintEvent *) override;
private:
  void paintStep(QPainter &p, int y, const QString &name, int done, int total, bool last);
  int thumbsDone_ = 0, thumbsTotal_ = 0, metaDone_ = 0, metaTotal_ = 0;
  QTimer showDelay_, idle_;
  QVariantAnimation fade_, spin_;
  qreal opacity_ = 0;
  LottieView *lottie_;
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
