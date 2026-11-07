#include "overlays.h"
#include "i18n.h"
#include "qatrace.h"
#include <QHBoxLayout>
#include <QPainter>
#include <QTextDocument>
#include <QtMath>
#include <QVBoxLayout>
LottieView::LottieView(const QString &name, int side, QWidget *parent) : QWidget(parent), movie_(QStringLiteral(":/lottie/%1.webp").arg(name)) {
  setFixedSize(side, side);
  setAttribute(Qt::WA_TransparentForMouseEvents);
  connect(&movie_, &QMovie::frameChanged, this, [this] { update(); });
}
void LottieView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.drawPixmap(rect(), movie_.currentPixmap());
}
// The animation runs only while it can be seen, and reduced motion keeps its first frame.
void LottieView::showEvent(QShowEvent *) {
  if (Theme::reduceMotion()) movie_.jumpToFrame(0);
  else movie_.start();
}
void LottieView::hideEvent(QHideEvent *) { movie_.stop(); }
HeroScreen::HeroScreen(QWidget *parent, const QString &lottie, bool overlay) : QWidget(parent), overlay_(overlay) {
  column_ = new QVBoxLayout(this);
  column_->setAlignment(Qt::AlignCenter);
  column_->setContentsMargins(0, 0, 0, overlay ? 8 : 32);
  column_->setSpacing(0);
  column_->addWidget(new LottieView(lottie, 104, this), 0, Qt::AlignHCenter);
  title_ = new QLabel;
  title_->setFont(Theme::font(20, QFont::DemiBold));
  title_->setAlignment(Qt::AlignCenter);
  title_->setStyleSheet(QStringLiteral("color:#ffffff;background:transparent"));
  title_->setWordWrap(true);
  subtitle_ = new QLabel;
  subtitle_->setFont(Theme::font(14));
  subtitle_->setAlignment(Qt::AlignCenter);
  subtitle_->setStyleSheet(QStringLiteral("color:#636363;background:transparent"));
  subtitle_->setWordWrap(true);
  subtitle_->setTextFormat(Qt::RichText);
  column_->addSpacing(8);
  column_->addWidget(title_, 0, Qt::AlignHCenter);
  column_->addSpacing(8);
  column_->addWidget(subtitle_, 0, Qt::AlignHCenter);
  column_->addSpacing(16);
  buttons_ = new QWidget;
  auto *row = new QHBoxLayout(buttons_);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(8);
  column_->addWidget(buttons_, 0, Qt::AlignHCenter);
}
void HeroScreen::setTexts(const QString &title, const QString &subtitle) {
  title_->setText(title);
  subtitle_->setText(QStringLiteral("<div style='line-height:%1%'>%2</div>").arg(subtitleLine_).arg(subtitle));
  subtitleRaw_ = subtitle;
  subtitle_->setVisible(!subtitle.isEmpty());
  fitTexts();
}
// A label takes its text width up to the limit, then wraps.
void HeroScreen::fitTexts() {
  for (QLabel *text : {title_, subtitle_}) {
    QTextDocument document;
    document.setDefaultFont(text->font());
    document.setHtml(text == title_ ? text->text() : subtitleRaw_);
    const int limit = text == title_ ? 384 : subtitleLimit_;
    const int width = qMin(limit, qCeil(document.idealWidth()) - 8);
    text->setFixedWidth(qMax(1, width));
    text->setFixedHeight(text->heightForWidth(text->width()));
  }
}
void HeroScreen::setSubtitleStyle(int pixelSize, const char *color) {
  subtitle_->setFont(Theme::font(pixelSize));
  subtitle_->setStyleSheet(QStringLiteral("color:%1;background:transparent").arg(QLatin1String(color)));
  subtitleLimit_ = 320;
  subtitleLine_ = pixelSize == 16 ? 126 : 120;
  setTexts(title_->text(), subtitleRaw_);
}
void HeroScreen::addButton(TextButton *button) { buttons_->layout()->addWidget(button); }
void HeroScreen::addVerticalButton(TextButton *button) {
  if (buttons_) {
    // The first one replaces the empty row.
    buttons_->hide();
    column_->removeWidget(buttons_);
    buttons_->deleteLater();
    buttons_ = nullptr;
  } else {
    column_->addSpacing(8);
  }
  column_->addWidget(button, 0, Qt::AlignHCenter);
}
void HeroScreen::paintEvent(QPaintEvent *) {
  QPainter p(this);
  if (overlay_) {
    if (!frost_.isNull()) p.drawPixmap(0, 0, frost_);
    p.fillRect(rect(), QColor(23, 23, 23, 204));
  } else {
    Theme::paintPage(p, rect(), mapTo(window(), QPoint(0, 0)));
  }
}
StartupScreen::StartupScreen(QWidget *parent) : QWidget(parent) {
  auto *column = new QVBoxLayout(this);
  column->setAlignment(Qt::AlignCenter);
  column->setSpacing(16);
  spinner_ = new Spinner(28, this);
  icon_ = new QLabel;
  icon_->setPixmap(Theme::icon("circle-x", 28, Theme::red400, devicePixelRatioF()));
  icon_->setStyleSheet(QStringLiteral("background:transparent"));
  title_ = new QLabel;
  title_->setFont(Theme::font(14, QFont::Medium));
  title_->setAlignment(Qt::AlignCenter);
  title_->setStyleSheet(QStringLiteral("color:#ffffff;background:transparent"));
  title_->setWordWrap(true);
  title_->setMaximumWidth(448);
  message_ = new QLabel;
  message_->setFont(Theme::font(14));
  message_->setAlignment(Qt::AlignCenter);
  message_->setStyleSheet(QStringLiteral("color:#636363;background:transparent"));
  message_->setWordWrap(true);
  message_->setMaximumWidth(448);
  retry_ = new TextButton(t("common.retry"), TextButton::Outline);
  retry_->setSmall(true);
  column->addWidget(spinner_, 0, Qt::AlignHCenter);
  column->addWidget(icon_, 0, Qt::AlignHCenter);
  column->addWidget(title_, 0, Qt::AlignHCenter);
  column->addWidget(message_, 0, Qt::AlignHCenter);
  column->addWidget(retry_, 0, Qt::AlignHCenter);
  connect(retry_, &QAbstractButton::clicked, this, &StartupScreen::retry);
  setBusy(t("startup.checking"));
}
void StartupScreen::setBusy(const QString &text) {
  spinner_->show();
  icon_->hide();
  retry_->hide();
  message_->hide();
  title_->setText(text);
}
void StartupScreen::setError(const QString &text, const QString &message) {
  spinner_->hide();
  icon_->show();
  retry_->show();
  title_->setText(text);
  message_->setText(message);
  message_->setVisible(!message.isEmpty());
}
void StartupScreen::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), Theme::background);
}
PrivacyShield::PrivacyShield(QWidget *parent) : QWidget(parent) {
  setAttribute(Qt::WA_OpaquePaintEvent);
  setFocusPolicy(Qt::NoFocus);
  hide();
}
void PrivacyShield::showFrom(QWidget *source) {
  hide();
  frost_ = Theme::frostedCopy(source, source->rect(), 24);
  setGeometry(parentWidget()->rect());
  show();
  raise();
}
void PrivacyShield::paintEvent(QPaintEvent *) {
  QPainter p(this);
  Theme::paintPage(p, rect(), QPoint(0, 0));
  if (!frost_.isNull()) {
    p.setOpacity(0.2);
    p.drawPixmap(0, 0, frost_);
  }
}
PreloadOverlay::PreloadOverlay(QWidget *parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents);  // The gallery stays usable underneath.
  hide();
  lottie_ = new LottieView(QStringLiteral("preloading"), 64, this);
  showDelay_.setSingleShot(true);
  showDelay_.setInterval(500);
  connect(&showDelay_, &QTimer::timeout, this, [this] {
    setGeometry(parentWidget()->rect());
    show();
    raise();
    fade_.stop();
    fade_.setStartValue(opacity_);
    fade_.setEndValue(1.0);
    fade_.start();
  });
  idle_.setSingleShot(true);
  idle_.setInterval(5000);
  connect(&idle_, &QTimer::timeout, this, &PreloadOverlay::finish);
  fade_.setDuration(Theme::motion(150));
  connect(&fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    opacity_ = v.toReal();
    update();
  });
  // The gallery stays live underneath: it scrolls and fills in while indexing runs.
  connect(&fade_, &QVariantAnimation::finished, this, [this] {
    if (opacity_ <= 0) hide();
  });
  spin_.setStartValue(0.0);
  spin_.setEndValue(360.0);
  spin_.setDuration(1000);
  spin_.setLoopCount(-1);
  connect(&spin_, &QVariantAnimation::valueChanged, this, [this] { if (isVisible()) update(QRect(width() / 2 - 250, height() / 2 - 40, 40, 200)); });
}
void PreloadOverlay::setStages(int thumbsDone, int thumbsTotal, int metaDone, int metaTotal) {
  thumbsDone_ = thumbsDone;
  thumbsTotal_ = thumbsTotal;
  metaDone_ = metaDone;
  metaTotal_ = metaTotal;
  idle_.start();
  if (!isVisible() && !showDelay_.isActive()) showDelay_.start();
  if (isVisible()) update();
  if (spin_.state() != QAbstractAnimation::Running && !Theme::reduceMotion()) spin_.start();
}
void PreloadOverlay::finish() {
  showDelay_.stop();
  idle_.stop();
  spin_.stop();
  if (!isVisible()) return;
  fade_.stop();
  fade_.setStartValue(opacity_);
  fade_.setEndValue(0.0);
  fade_.start();
}
void PreloadOverlay::paintStep(QPainter &p, int y, const QString &name, int done, int total, bool last) {
  const int left = width() / 2 - 250;
  const bool complete = total == 0 || done >= total;
  const bool active = !complete && done > 0;
  const QRectF circle(left, y, 24, 24);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(complete || active ? Theme::primary : Theme::border, 1));
  p.setBrush(complete ? Theme::primary : QColor(Qt::transparent));
  p.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));
  if (complete) {
    Theme::drawIcon(p, "check", QRectF(left + 6, y + 6, 12, 12), Theme::onPrimary);
  } else if (active) {
    p.save();
    p.translate(circle.center());
    p.rotate(spin_.currentValue().toReal());
    Theme::drawIcon(p, "loader-2", QRectF(-6, -6, 12, 12), Theme::primary);
    p.restore();
  }
  // From under the circle to the next one.
  if (!last) p.fillRect(QRectF(left + 11.5, y + 24, 1, 40), QColor(255, 255, 255, 15));
  const int textLeft = left + 40, textWidth = 460;
  p.setFont(Theme::font(16, QFont::Medium));
  p.setPen(Theme::text);
  p.drawText(QRect(textLeft, y, textWidth - 90, 24), Qt::AlignVCenter | Qt::AlignLeft, name);
  p.setFont(Theme::font(12, QFont::Medium));
  p.setPen(Theme::muted);
  p.drawText(QRect(textLeft, y, textWidth, 24), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("%1 / %2").arg(done).arg(total));
  const QRectF track(textLeft, y + 36, textWidth, 4);
  p.setPen(Qt::NoPen);
  p.setBrush(Theme::secondary);
  p.drawRoundedRect(track, 2, 2);
  p.setBrush(complete || active ? Theme::primary : QColor(99, 99, 99, 102));
  p.drawRoundedRect(QRectF(track.x(), track.y(), track.width() * (total ? qBound(0.0, double(done) / total, 1.0) : 1), 4), 2, 2);
}
void PreloadOverlay::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setOpacity(opacity_);
  p.fillRect(rect(), QColor(23, 23, 23, 204));
  const int left = width() / 2 - 250, top = height() / 2 - 120;
  lottie_->move(left, top);
  p.setRenderHint(QPainter::Antialiasing);
  p.setFont(Theme::font(20, QFont::DemiBold));
  p.setPen(Theme::text);
  p.drawText(QRect(left + 80, top + 2, 420, 26), Qt::AlignVCenter | Qt::AlignLeft, t("preload.title"));
  const int done = thumbsDone_ + metaDone_, total = thumbsTotal_ + metaTotal_;
  constexpr qreal unit = 24.0 / 36, radius = 15.9155 * unit;
  const QRectF ring(left + 80 + 12 - radius, top + 32 + 12 - radius, 2 * radius, 2 * radius);
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(Theme::secondary, 4 * unit));
  p.drawEllipse(ring);
  p.setPen(QPen(Theme::primary, 4 * unit, Qt::SolidLine, Qt::RoundCap));
  p.drawArc(ring, 90 * 16, -int((total ? double(done) / total : 0) * 360 * 16));
  p.setFont(Theme::font(14, QFont::Medium));
  p.setPen(Theme::muted);
  p.drawText(QRect(left + 80 + 32, top + 32, 380, 24), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("%1 / %2").arg(done).arg(total));
  paintStep(p, top + 88, t("preload.thumbnails"), thumbsDone_, thumbsTotal_, false);
  paintStep(p, top + 88 + 64, t("preload.metadata"), metaDone_, metaTotal_, true);
}
LoggerPanel::LoggerPanel(QWidget *parent) : QWidget(parent) {
  setFixedSize(512, 128);
  hide();
  refresh_.setInterval(500);
  connect(&refresh_, &QTimer::timeout, this, [this] { if (isVisible()) update(); });
  refresh_.start();
}
void LoggerPanel::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(Theme::border, 1));
  p.setBrush(QColor(0, 0, 0, 200));
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRadius2xl, Theme::kRadius2xl);
  QFont mono(QStringLiteral("Menlo"));
  mono.setStyleHint(QFont::Monospace);
  mono.setPixelSize(10);
  p.setFont(mono);
  const QStringList lines = qaLogLines();
  int y = height() - 8;
  for (int i = int(lines.size()) - 1; i >= 0 && y > 8; i--) {
    y -= 14;
    p.setPen(lines[i].contains(QLatin1String("error"), Qt::CaseInsensitive) ? Theme::red400 : QColor(255, 255, 255, 204));
    p.drawText(QRect(8, y, width() - 16, 14), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(lines[i], Qt::ElideRight, width() - 16));
  }
}
