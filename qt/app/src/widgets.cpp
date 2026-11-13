#include "widgets.h"
#include "i18n.h"
#include <QApplication>
#include <QAccessible>
#include <QKeyEvent>
#include <QIntValidator>
#include <QLocale>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QPointer>
#include <QPainterPath>
#include <QResizeEvent>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
QPoint hostPoint(QWidget *host, QWidget *from, const QPoint &point) { return from->mapTo(host, point); }
QRect hostRect(QWidget *host, QWidget *from) { return QRect(from->mapTo(host, QPoint(0, 0)), from->size()); }
static void focusFirstControl(QWidget *content);
static bool keyboard_ = false;  // The last input was a key.
namespace {
class FocusRing : public QWidget {
public:
  FocusRing() { setAttribute(Qt::WA_TransparentForMouseEvents); setFocusPolicy(Qt::NoFocus); }
  void follow(QWidget *target) {
    if (target_) target_->removeEventFilter(this);
    target_ = target;
    if (!target) return hide();
    setParent(target->parentWidget());
    target->installEventFilter(this);
    place();
    show();
    raise();
  }
protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::Move || event->type() == QEvent::Resize) place();
    if (event->type() == QEvent::Hide) hide();
    if (event->type() == QEvent::Show) show();
    return false;
  }
  void paintEvent(QPaintEvent *) override {
    if (!target_) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal radius = qMin<qreal>(target_->property("ringRadius").isValid() ? target_->property("ringRadius").toReal() : Theme::kRadius3xl, target_->height() / 2.0);
    QPen pen(QColor(115, 115, 115, 128), 3);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), radius + 1.5, radius + 1.5);
    p.setPen(QPen(QColor(115, 115, 115), 1));
    p.drawRoundedRect(QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5), radius - 0.5, radius - 0.5);
  }
private:
  void place() { if (target_) setGeometry(target_->geometry().adjusted(-3, -3, 3, 3)); }
  QPointer<QWidget> target_;
};
class FocusModality : public QObject {
public:
  explicit FocusModality(QObject *parent) : QObject(parent) {
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) { update(now); });
  }
protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::KeyPress) {
      if (!keyboard_) {
        keyboard_ = true;
        update(QApplication::focusWidget());
      }
    } else if (event->type() == QEvent::MouseButtonPress && keyboard_) {
      keyboard_ = false;
      if (ring_) ring_->follow(nullptr);
    }
    return false;
  }
private:
  void update(QWidget *now) {
    const bool control = qobject_cast<IconButton *>(now) || qobject_cast<TextButton *>(now) || qobject_cast<Switch *>(now);
    if (!(keyboard_ && control)) {
      if (ring_) ring_->follow(nullptr);
      return;
    }
    if (!ring_) ring_ = new FocusRing;  // Owned by whichever panel it is reparented to; a closed panel takes it along.
    ring_->follow(now);
  }
  QPointer<FocusRing> ring_;
};
}
bool focusVisible() { return keyboard_; }
void installFocusRing() {
  auto *modality = new FocusModality(qApp);
  qApp->installEventFilter(modality);
}
Glide::Glide(QWidget *owner, int milliseconds, qreal start) : QObject(owner), owner_(owner), duration_(milliseconds), value_(start), target_(start) {
  animation_.setEasingCurve(QEasingCurve::OutCubic);
  connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    value_ = v.toReal();
    owner_->update();
  });
}
void Glide::to(qreal target) {
  if (qFuzzyCompare(target_, target) && (animation_.state() == QAbstractAnimation::Running || qFuzzyCompare(value_ + 1, target + 1))) return;
  target_ = target;
  const int duration = Theme::motion(duration_);
  animation_.stop();
  if (duration == 0 || !owner_->isVisible()) {
    value_ = target;
    owner_->update();
    return;
  }
  animation_.setDuration(duration);
  animation_.setStartValue(value_);
  animation_.setEndValue(target);
  animation_.start();
}
void Glide::set(qreal value) {
  animation_.stop();
  value_ = target_ = value;
  owner_->update();
}
PressScale::PressScale(QWidget *owner, qreal hover, qreal press) : QObject(owner), owner_(owner), hover_(hover), press_(press) {
  tick_.setTimerType(Qt::PreciseTimer);
  tick_.setInterval(8);
  connect(&tick_, &QTimer::timeout, this, [this] {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool moving = spring_.step(qMin<qint64>(now - last_, 50) / 1000.0);
    last_ = now;
    owner_->update();
    if (!moving) tick_.stop();
  });
}
void PressScale::retarget() {
  spring_.target = pressed_ ? press_ : hovered_ ? hover_ : 1;
  if (Theme::reduceMotion()) {
    spring_.value = spring_.target;
    spring_.velocity = 0;
    owner_->update();
    return;
  }
  if (!tick_.isActive()) {
    last_ = QDateTime::currentMSecsSinceEpoch();
    tick_.start();
  }
}
void IconButton::setPressScale(bool on) {
  if (on && !scale_) scale_ = new PressScale(this);
  if (!on && scale_) {
    delete scale_;
    scale_ = nullptr;
  }
  update();
}
void IconButton::enterEvent(QEnterEvent *) {
  hover_.to(1);
  if (scale_) scale_->setHovered(true);
}
void IconButton::leaveEvent(QEvent *) {
  if (!held_) hover_.to(0);
  if (scale_) scale_->setHovered(false);
}
void IconButton::mousePressEvent(QMouseEvent *event) {
  QAbstractButton::mousePressEvent(event);
  if (scale_ && event->button() == Qt::LeftButton) scale_->setPressed(true);
}
void IconButton::mouseReleaseEvent(QMouseEvent *event) {
  if (scale_) scale_->setPressed(false);
  QAbstractButton::mouseReleaseEvent(event);
}
IconButton::IconButton(const QByteArray &icon, const QString &label, int side, QWidget *parent) : QAbstractButton(parent), icon_(icon), side_(side), iconSize_(16), hover_(this, 150) {
  setFixedSize(side, side);
  setLabel(label);
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::PointingHandCursor);
}
void IconButton::setLabel(const QString &label) {
  setAccessibleName(label);
  setToolTip(label);
}
void IconButton::changeEvent(QEvent *event) {
  QAbstractButton::changeEvent(event);
  if (event->type() == QEvent::EnabledChange) update();
}
void IconButton::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (scale_) {
    // Drawn 2 px inside the edge, so the grown button still fits the widget.
    const qreal k = scale_->value() * (width() - 4.0) / width();
    p.translate(width() / 2.0, height() / 2.0);
    p.scale(k, k);
    p.translate(-width() / 2.0, -height() / 2.0);
  }
  const qreal radius = radius_ >= 0 ? radius_ : qMin(width(), height()) / 2.0;
  QColor fill = hoverFill_;
  fill.setAlphaF(fill.alphaF() * hover_.value());
  if (base_.isValid()) fill = Theme::mix(base_, hoverFill_, hover_.value());
  if (isChecked() || fill_) fill = QColor(255, 255, 255, 31);
  if (activeFill_.isValid()) {
    p.setPen(QPen(activeBorder_, 1));
    p.setBrush(activeFill_);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
  } else if (fill.alpha()) {
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(rect(), radius, radius);
  }
  QColor color = tint_.isValid() ? tint_ : Theme::mix(idle_, hoverInk_, hover_.value());
  if (!isEnabled()) color.setAlphaF(color.alphaF() * 0.5);
  const int s = iconSize_;
  if (angle_ != 0) {
    p.translate(width() / 2.0, height() / 2.0);
    p.rotate(angle_);
    p.translate(-width() / 2.0, -height() / 2.0);
  }
  Theme::drawIcon(p, icon_, QRectF((width() - s) / 2.0, (height() - s) / 2.0, s, s), color);
}
void IconButton::setHeld(bool held) {
  held_ = held;
  hover_.to(held || underMouse() ? 1 : 0);
}
TextButton::TextButton(const QString &label, Kind kind, QWidget *parent) : QAbstractButton(parent), kind_(kind), hover_(this, 150) {
  setText(label);
  setAccessibleName(label);
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::PointingHandCursor);
  setFont(Theme::font(14, QFont::Medium));
  spin_.setStartValue(0.0);
  spin_.setEndValue(360.0);
  spin_.setDuration(800);
  spin_.setLoopCount(-1);
  connect(&spin_, &QVariantAnimation::valueChanged, this, [this] { update(); });
}
void TextButton::setSmall(bool small) {
  small_ = small;
  setFont(Theme::font(small ? 12 : 14, QFont::Medium));
  updateGeometry();
  update();
}
QSize TextButton::sizeHint() const {
  const bool hasIcon = !icon_.isEmpty() || busy_;
  const int pad = small_ ? (hasIcon ? 10 : 12) : (hasIcon ? 12 : 16);
  const int gap = small_ ? 6 : 8;
  return {fontMetrics().horizontalAdvance(text()) + (trailing_.isEmpty() ? 2 * pad : pad + 8) + (hasIcon ? 16 + gap : 0) + (trailing_.isEmpty() ? 0 : 16 + gap) + (leading_.isNull() ? 0 : leadingSize_.width() + 8), small_ ? 30 : 36};
}
void TextButton::setBusy(bool busy) {
  busy_ = busy;
  setEnabled(!busy);
  if (busy) spin_.start();
  else spin_.stop();
  updateGeometry();
  update();
}
void TextButton::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const qreal h = hover_.value();
  QColor fill(Qt::transparent), ink = Theme::text, line(Qt::transparent);
  switch (kind_) {
    case Primary: {
      fill = Theme::primary;
      fill.setAlphaF(1 - 0.1 * h);
      ink = Theme::onPrimary;
    } break;
    case Outline:
      fill = Theme::mix(QColor(23, 23, 23, 77), Theme::hover, h);
      line = Theme::border;
      break;
    case Ghost:
      fill = Theme::hover;
      fill.setAlphaF(fill.alphaF() * h);
      // A checked toggle (the duplicates review) keeps its pressed look.
      if (isCheckable() && isChecked()) fill = Theme::mix(QColor(255, 255, 255, 26), Theme::hover, h);
      if (navigation_) {
        fill = isChecked() ? QColor(255, 255, 255, 26) : QColor(255, 255, 255, int(13 * h));
        ink = isChecked() ? Theme::text : Theme::mix(Theme::muted, Theme::text, h);
      }
      if (rowHover_.isValid()) {
        fill = rowHover_;
        fill.setAlphaF(fill.alphaF() * h);
        ink = rowInk_;
      }
      break;
    case Danger:
      fill = Theme::red500;
      fill.setAlphaF(0.3 + 0.2 * h);
      break;
  }
  if (!isEnabled()) p.setOpacity(0.5);
  const qreal radius = qMin<qreal>(radius_, height() / 2.0);
  p.setPen(line.alpha() ? QPen(line, 1) : QPen(Qt::NoPen));
  p.setBrush(fill);
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
  p.setFont(font());
  const int gap = small_ ? 6 : 8;
  const int textWidth = fontMetrics().horizontalAdvance(text());
  const bool hasIcon = !icon_.isEmpty() || busy_;
  const int total = textWidth + (hasIcon ? 16 + gap : 0) + (trailing_.isEmpty() ? 0 : 16 + gap) + (leading_.isNull() ? 0 : leadingSize_.width() + 8);
  int x = left_ ? (hasIcon ? 12 : 16) : qMax(4, (width() - total) / 2);
  if (navigation_) x = 10;
  if (inset_ >= 0) x = inset_;
  if (!leading_.isNull()) {
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawPixmap(QRect(x, (height() - leadingSize_.height()) / 2, leadingSize_.width(), leadingSize_.height()), leading_);
    x += leadingSize_.width() + 8;
  }
  if (hasIcon) {
    const QRectF box(x, (height() - 16) / 2.0, 16, 16);
    if (busy_) {
      p.save();
      p.translate(box.center());
      p.rotate(spin_.currentValue().toReal());
      Theme::drawIcon(p, "loader-2", QRectF(-8, -8, 16, 16), ink);
      p.restore();
    } else {
      Theme::drawIcon(p, icon_, box, iconInk_.isValid() ? iconInk_ : ink);
    }
    x += 16 + gap;
  }
  p.setPen(ink);
  const int right = trailing_.isEmpty() ? 4 : 16 + 8;
  p.drawText(QRect(x, 0, width() - x - right, height()), Qt::AlignVCenter | Qt::AlignLeft, Theme::elided(font(), text(), width() - x - right));
  if (!trailing_.isEmpty()) Theme::drawIcon(p, trailing_, QRectF(width() - 16 - 8, (height() - 16) / 2.0, 16, 16), Theme::muted);
  if (dot_) {
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::amber400);
    p.drawEllipse(QRectF(width() - 16, height() / 2.0 - 3, 6, 6));
  }
}
Switch::Switch(const QString &label, QWidget *parent) : QAbstractButton(parent), knob_(this, 200) {
  setCheckable(true);
  setFixedSize(36, 20);
  setAccessibleName(label);
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::PointingHandCursor);
  connect(this, &QAbstractButton::toggled, this, [this](bool on) { knob_.to(on ? 1 : 0); });
}
void Switch::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const qreal t = knob_.value();
  if (!isEnabled()) p.setOpacity(0.5);
  p.setPen(Qt::NoPen);
  p.setBrush(Theme::mix(QColor(255, 255, 255, 38), Theme::emerald500, t));
  p.drawRoundedRect(rect(), 10, 10);
  p.setBrush(Qt::white);
  p.drawEllipse(QRectF(2 + t * 16 - t * 0, 2, 16, 16));
}
TextField::TextField(const QString &text, QWidget *parent) : QLineEdit(text, parent) {
  setAttribute(Qt::WA_MacShowFocusRect, false);
  setMetrics(36);
}
void TextField::setMetrics(int height, int fontPixels) {
  setStyleSheet(QStringLiteral("QLineEdit{border:none;background:transparent;padding:0 13px;min-height:%1px;max-height:%1px;font-size:%2px;}").arg(height).arg(fontPixels));
}
void TextField::paintEvent(QPaintEvent *event) {
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = radius_ < 0 ? frame.height() / 2 : radius_;
    p.setPen(QPen(hasFocus() ? QColor(0x73, 0x73, 0x73) : QColor(255, 255, 255, isEnabled() ? 26 : 13), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(frame, radius, radius);
  }
  QLineEdit::paintEvent(event);
}
void TextField::focusInEvent(QFocusEvent *event) {
  QLineEdit::focusInEvent(event);
  update();
}
void TextField::focusOutEvent(QFocusEvent *event) {
  QLineEdit::focusOutEvent(event);
  update();
}
NumberSlider::NumberSlider(int low, int high, int value, const QString &label, QWidget *parent) : QWidget(parent), low_(low), high_(high), value_(value), position_(this, 110, value) {
  setFixedSize(144, 36);
  setAccessibleName(label);
  setToolTip(label);
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::OpenHandCursor);  // 6.x: cursor-grab on the slider.
  setAttribute(Qt::WA_Hover);
}
void NumberSlider::setValue(int value, bool notify) {
  value = qBound(low_, value, high_);
  if (value == value_) return;
  value_ = value;
  position_.to(value);
  if (notify) emit valueChanged(value);
  update();
}
void NumberSlider::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  // Track, range and a pill thumb carrying the value.
  const QString label = QString::number(value_);
  QFont font = Theme::font(11, QFont::Medium);
  const int thumbWidth = qMax(24, QFontMetrics(font).horizontalAdvance(label) + 14), thumbHeight = 22;
  const qreal left = 8, right = width() - 8, cy = height() / 2.0;
  const qreal t = (position_.value() - low_) / qMax(1, high_ - low_), x = left + thumbWidth / 2.0 + (right - left - thumbWidth) * t;
  if (!isEnabled()) p.setOpacity(0.5);
  p.setPen(Qt::NoPen);
  p.setBrush(Theme::secondary);
  p.drawRoundedRect(QRectF(left, cy - 3, right - left, 6), 3, 3);
  p.setBrush(Theme::primary);
  p.drawRoundedRect(QRectF(left, cy - 3, x - left, 6), 3, 3);
  const QRectF thumb(x - thumbWidth / 2.0, cy - thumbHeight / 2.0, thumbWidth, thumbHeight);
  if (underMouse() || hasFocus()) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 26));
    p.drawRoundedRect(thumb.adjusted(-4, -4, 4, 4), thumbHeight / 2.0 + 4, thumbHeight / 2.0 + 4);
  }
  QColor line = Theme::primary;
  line.setAlphaF(0.8);
  p.setPen(QPen(line, 1));
  p.setBrush(Theme::background);
  p.drawRoundedRect(thumb.adjusted(0.5, 0.5, -0.5, -0.5), thumbHeight / 2.0, thumbHeight / 2.0);
  p.setPen(Theme::text);
  p.setFont(font);
  p.drawText(thumb, Qt::AlignCenter, label);
}
void NumberSlider::fromPosition(qreal x) {
  const qreal inset = 8 + 12, t = (x - inset) / qMax<qreal>(1, width() - 2 * inset);
  setValue(low_ + qRound(qBound<qreal>(0, t, 1) * (high_ - low_)), true);
}
void NumberSlider::mousePressEvent(QMouseEvent *event) {
  setCursor(Qt::ClosedHandCursor);
  fromPosition(event->position().x());
}
void NumberSlider::mouseReleaseEvent(QMouseEvent *) { setCursor(Qt::OpenHandCursor); }
void NumberSlider::mouseMoveEvent(QMouseEvent *event) { if (event->buttons() & Qt::LeftButton) fromPosition(event->position().x()); }
void NumberSlider::wheelEvent(QWheelEvent *event) {
  wheelAccumulator_ += event->angleDelta().y();
  if (qAbs(wheelAccumulator_) >= 120) {
    setValue(value_ + (wheelAccumulator_ > 0 ? 1 : -1), true);
    wheelAccumulator_ = 0;
  }
  event->accept();
}
void NumberSlider::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
    case Qt::Key_Left: case Qt::Key_Down: setValue(value_ - 1, true); break;
    case Qt::Key_Right: case Qt::Key_Up: setValue(value_ + 1, true); break;
    case Qt::Key_Home: setValue(low_, true); break;
    case Qt::Key_End: setValue(high_, true); break;
    default: QWidget::keyPressEvent(event);
  }
}
Segmented::Segmented(const QVector<SegmentOption> &options, const QString &label, QWidget *parent) : QWidget(parent), options_(options) {
  setFixedSize(int(options.size()) * 38 - 2, 36);
  setAccessibleName(label);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setCursor(Qt::PointingHandCursor);
}
void Segmented::setLabels(const QString &label, const QStringList &optionLabels) {
  setAccessibleName(label);
  for (int i = 0; i < options_.size() && i < optionLabels.size(); i++) options_[i].label = optionLabels[i];
}
void Segmented::setCurrent(int index, bool notify) {
  if (index < 0 || index >= options_.size()) return;
  const bool changed = index != current_;
  current_ = index;
  if (changed && notify) emit activated(index);
  update();
}
bool Segmented::event(QEvent *event) {
  if (event->type() == QEvent::ToolTip) {
    auto *help = static_cast<QHelpEvent *>(event);
    const int index = help->pos().x() / 38;
    if (index >= 0 && index < options_.size()) QToolTip::showText(help->globalPos(), options_[index].label, this);
    return true;
  }
  return QWidget::event(event);
}
void Segmented::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (!isEnabled()) p.setOpacity(0.4);
  for (int i = 0; i < options_.size(); i++) {
    const QRectF slot(i * 38, 2, 36, 32);
    if (i == current_) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(255, 255, 255, 31));
      p.drawRoundedRect(slot, Theme::kRadiusLg, Theme::kRadiusLg);
      if (hasFocus()) {
        p.setPen(QPen(QColor(255, 255, 255, 89), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(slot.adjusted(1, 1, -1, -1), Theme::kRadiusLg - 1, Theme::kRadiusLg - 1);
      }
    }
    Theme::drawIcon(p, options_[i].icon, QRectF(slot.center().x() - 8, slot.center().y() - 8, 16, 16), i == current_ || i == hovered_ ? Theme::text : Theme::muted);
  }
}
void Segmented::mousePressEvent(QMouseEvent *event) { setCurrent(qBound(0, int(event->position().x()) / 38, int(options_.size()) - 1), true); }
void Segmented::mouseMoveEvent(QMouseEvent *event) {
  const int hovered = qBound(0, int(event->position().x()) / 38, int(options_.size()) - 1);
  if (hovered != hovered_) { hovered_ = hovered; update(); }
}
void Segmented::leaveEvent(QEvent *) { hovered_ = -1; update(); }
void Segmented::keyPressEvent(QKeyEvent *event) {
  const int count = options_.size();
  switch (event->key()) {
    case Qt::Key_Right: case Qt::Key_Down: setCurrent((current_ + 1) % count, true); break;
    case Qt::Key_Left: case Qt::Key_Up: setCurrent((current_ + count - 1) % count, true); break;
    case Qt::Key_Home: setCurrent(0, true); break;
    case Qt::Key_End: setCurrent(count - 1, true); break;
    default: QWidget::keyPressEvent(event);
  }
}
Spinner::Spinner(int side, QWidget *parent) : QWidget(parent), side_(side) {
  setFixedSize(side, side);
  spin_.setStartValue(0.0);
  spin_.setEndValue(360.0);
  spin_.setDuration(1000);
  spin_.setLoopCount(-1);
  connect(&spin_, &QVariantAnimation::valueChanged, this, [this] { update(); });
}
void Spinner::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.translate(width() / 2.0, height() / 2.0);
  p.rotate(spin_.currentValue().toReal());
  Theme::drawIcon(p, "loader-2", QRectF(-side_ / 2.0, -side_ / 2.0, side_, side_), Theme::muted);
}
void Spinner::showEvent(QShowEvent *) { if (!Theme::reduceMotion()) spin_.start(); }
void Spinner::hideEvent(QHideEvent *) { spin_.stop(); }
void paintProgressRing(QPainter &painter, const QRectF &rect, qreal fraction, const QColor &color, qreal stroke) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(Qt::NoBrush);
  QColor track = color;
  track.setAlphaF(0.22);
  painter.setPen(QPen(track, stroke));
  painter.drawEllipse(rect);
  painter.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap));
  painter.drawArc(rect, 90 * 16, -int(qBound<qreal>(0, fraction, 1) * 360 * 16));
  painter.restore();
}
Popover::Popover(QWidget *host, Surface surface) : QWidget(host), surface_(surface) {
  hide();
  content_ = new QWidget(this);
  connect(&fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    progress_ = v.toReal();
    update(panel_.adjusted(-30, -30, 30, 30));
    if (dim_) update();
  });
  connect(&fade_, &QVariantAnimation::finished, this, [this] {
    frame_ = {};
    if (closing_) return closeNow();
    progress_ = 1;
    content_->show();
    content_->setFocus();
    focusFirstControl(content_);
    update();
  });
}
void Popover::setPanelSize(const QSize &size) {
  panel_.setSize(size);
  if (open_) place();
}
void Popover::place() {
  QWidget *host = parentWidget();
  setGeometry(host->rect());
  QRect r = panel_;
  const int gap = 6;
  switch (side_) {
    case Qt::BottomEdge: r.moveTop(anchor_.bottom() + gap); break;
    case Qt::TopEdge: r.moveBottom(anchor_.top() - gap); break;
    case Qt::RightEdge: r.moveLeft(anchor_.right() + gap); break;
    case Qt::LeftEdge: r.moveRight(anchor_.left() - gap); break;
  }
  if (side_ == Qt::BottomEdge || side_ == Qt::TopEdge) {
    if (align_ & Qt::AlignRight) r.moveRight(anchor_.right());
    else if (align_ & Qt::AlignHCenter) r.moveLeft(anchor_.center().x() - r.width() / 2);
    else r.moveLeft(anchor_.left());
  } else {
    if (align_ & Qt::AlignBottom) r.moveBottom(anchor_.bottom());
    else if (align_ & Qt::AlignVCenter) r.moveTop(anchor_.center().y() - r.height() / 2);
    else r.moveTop(anchor_.top());
  }
  const QRect bounds = host->rect().adjusted(8, 8, -8, -8);
  // Prefer the opposite side over overlapping the anchor.
  if (side_ == Qt::RightEdge && r.right() > bounds.right()) r.moveRight(anchor_.left() - gap);
  if (side_ == Qt::LeftEdge && r.left() < bounds.left()) r.moveLeft(anchor_.right() + gap);
  if (side_ == Qt::BottomEdge && r.bottom() > bounds.bottom() && anchor_.top() - gap - r.height() >= bounds.top()) r.moveBottom(anchor_.top() - gap);
  if (side_ == Qt::TopEdge && r.top() < bounds.top() && anchor_.bottom() + gap + r.height() <= bounds.bottom()) r.moveTop(anchor_.bottom() + gap);
  r.moveLeft(qBound(bounds.left(), r.left(), qMax(bounds.left(), bounds.right() - r.width())));
  r.moveTop(qBound(bounds.top(), r.top(), qMax(bounds.top(), bounds.bottom() - r.height())));
  panel_ = r;
  content_->setGeometry(contentArea());
  // A submenu or side panel covers only itself: the menu it came from keeps its hover and clicks, so moving to another
  // item switches the submenu and a click there acts on the first press.
  if (parentPopover_ && !dim_) setMask(QRegion(panel_.adjusted(-28, -28, 28, 28)) - QRegion(parentPopover_->panel_));
  else clearMask();
  update();
}
QRect Popover::contentArea() const { return panel_.adjusted(1, 1, -1, -1); }
void Popover::resizeEvent(QResizeEvent *) { content_->setGeometry(contentArea()); }
// An underdamped spring over the unit interval: about 8% overshoot near a third of the way, settled at the end
// (6.x popovers: framer-motion spring, bounce 0.2).
static qreal springEase(qreal t) {
  constexpr qreal zeta = 0.62, omega = 13.0;
  const qreal damped = omega * std::sqrt(1 - zeta * zeta);
  return 1 - std::exp(-zeta * omega * t) * (std::cos(damped * t) + zeta * omega / damped * std::sin(damped * t));
}
void Popover::startMotion(bool opening) {
  fade_.stop();
  const int duration = Theme::motion(surface_ == DialogSurface ? 150 : 200);
  if (!duration) {
    progress_ = opening ? 1 : 0;
    if (!opening) closeNow();
    return;
  }
  // A picture of the finished panel moves; the widgets inside appear when it has landed.
  frame_ = QPixmap(panel_.size() * devicePixelRatioF());
  frame_.setDevicePixelRatio(devicePixelRatioF());
  frame_.fill(Qt::transparent);
  {
    QPainter p(&frame_);
    p.translate(-panel_.topLeft());
    paintSurface(p);
    p.translate(panel_.topLeft());
    content_->show();
    content_->render(&p, content_->pos() - panel_.topLeft(), QRegion(), QWidget::DrawChildren);
  }
  content_->hide();
  const bool spring = surface_ == PopoverSurface && opening;
  if (spring) {
    QEasingCurve curve;
    curve.setCustomType(springEase);
    fade_.setEasingCurve(curve);
  } else {
    fade_.setEasingCurve(surface_ == ContextSurface ? Theme::bezier(0.22, 1, 0.36, 1) : QEasingCurve(QEasingCurve::OutCubic));
  }
  fade_.setDuration(spring ? Theme::motion(350) : duration);
  fade_.setStartValue(opening ? 0.0 : progress_);
  fade_.setEndValue(opening ? 1.0 : 0.0);
  fade_.start();
}
// Focus moves to the first tabbable control of the content.
static void focusFirstControl(QWidget *content) {
  for (QWidget *next = content->nextInFocusChain(); next && next != content; next = next->nextInFocusChain()) {
    if (content->isAncestorOf(next) && (next->focusPolicy() & Qt::TabFocus) && next->isEnabled() && next->isVisibleTo(content)) {
      next->setFocus(Qt::PopupFocusReason);
      return;
    }
  }
}
void Popover::openAt(const QRect &anchor, Qt::Edge side, Qt::Alignment align) {
  closing_ = false;
  anchor_ = anchor;
  side_ = side;
  align_ = align;
  aboutToOpen();
  restoreFocus_ = QApplication::focusWidget();
  place();
  if (surface_ != DialogSurface) frost_ = Theme::frostedCopy(parentWidget(), panel_, 24);
  open_ = true;
  raise();
  show();
  qApp->installEventFilter(this);
  progress_ = 0;
  startMotion(true);
  if (!Theme::motion(1)) {
    progress_ = 1;
    content_->setFocus();
    focusFirstControl(content_);
  }
}
void Popover::animateClose() {
  if (!open_ || closing_) return;
  if (!Theme::motion(1)) return closeNow();
  closing_ = true;
  qApp->removeEventFilter(this);
  startMotion(false);
}
void Popover::closeNow() {
  if (!open_) return;
  open_ = false;
  closing_ = false;
  fade_.stop();
  frame_ = {};
  qApp->removeEventFilter(this);
  hide();
  if (trigger_ && trigger_->isVisible()) trigger_->setFocus(Qt::PopupFocusReason);
  else if (restoreFocus_ && restoreFocus_->isVisible()) restoreFocus_->setFocus();
  emit closed();
}
void Popover::closeAll(QWidget *host) {
  for (Popover *popover : host->findChildren<Popover *>(Qt::FindDirectChildrenOnly)) popover->closeNow();
}
bool Popover::eventFilter(QObject *object, QEvent *event) {
  if (event->type() == QEvent::MouseButtonPress && dismissOnOutside_ && object->isWidgetType()) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    const QPoint p = mapFromGlobal(mouse->globalPosition().toPoint());
    if (!panel_.contains(p)) {
      // A side panel keeps its parent; a press on a panel stacked above this one is not outside.
      for (Popover *other : parentWidget()->findChildren<Popover *>(Qt::FindDirectChildrenOnly)) {
        if (other != this && other->isOpen() && other->parentPopover_ == this && other->panel_.contains(p)) return false;
      }
      // A press on the menu this one came from is that menu's: it may open another item at once.
      if (parentPopover_ && parentPopover_->panel_.contains(p)) {
        closeNow();
        return false;
      }
      // A right click elsewhere asks for the menu of what is under it, not a second click.
      if (mouse->button() == Qt::RightButton && !dim_) {
        const QPoint global = mouse->globalPosition().toPoint();
        for (Popover *other : parentWidget()->findChildren<Popover *>(Qt::FindDirectChildrenOnly))
          if (other->isOpen() && !other->dim_) other->closeNow();
        if (QWidget *under = QApplication::widgetAt(global)) {
          QContextMenuEvent request(QContextMenuEvent::Mouse, under->mapFromGlobal(global), global, mouse->modifiers());
          QApplication::sendEvent(under, &request);
        }
        return true;
      }
      animateClose();
      return dim_;
    }
  } else if (event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Escape) {
      for (Popover *other : parentWidget()->findChildren<Popover *>(Qt::FindDirectChildrenOnly))
        if (other != this && other->isOpen() && other->parentPopover_ == this) return false;
      animateClose();
      return true;
    }
  }
  return false;
}
void Popover::paintSurface(QPainter &p) {
  p.setRenderHint(QPainter::Antialiasing);
  if (surface_ == ContextSurface) {
    QPainterPath shape;
    shape.addRoundedRect(QRectF(panel_).adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRadius2xl, Theme::kRadius2xl);
    p.save();
    p.setClipPath(shape);
    if (!frost_.isNull()) p.drawPixmap(panel_.topLeft(), frost_);
    p.fillRect(panel_, QColor(23, 23, 23, 204));
    p.restore();
    p.setPen(QPen(Theme::border, 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
  } else if (surface_ == PopoverSurface && !frost_.isNull()) {
    // Frosted like the context menus, a little denser (6.x: bg-popover/90 with a backdrop blur).
    QPainterPath shape;
    shape.addRoundedRect(QRectF(panel_).adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRadius2xl, Theme::kRadius2xl);
    p.save();
    p.setClipPath(shape);
    p.drawPixmap(panel_.topLeft(), frost_);
    QColor fill = Theme::popover;
    fill.setAlpha(230);
    p.fillRect(panel_, fill);
    p.restore();
    p.setPen(QPen(Theme::border, 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
  } else {
    p.setPen(QPen(Theme::border, 1));
    p.setBrush(Theme::popover);
    p.drawRoundedRect(QRectF(panel_).adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRadius2xl, Theme::kRadius2xl);
  }
}
void Popover::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  if (dim_) p.fillRect(rect(), QColor(0, 0, 0, int(128 * progress_)));
  const bool moving = !frame_.isNull();
  const qreal t = moving ? progress_ : 1;
  p.save();
  if (moving) {
    // Popovers zoom from 96% and slide 4px; dialogs zoom from 95% at the centre; a context menu also starts at 40% opacity.
    const bool context = surface_ == ContextSurface, popover = surface_ == PopoverSurface;
    // Popovers grow from 60% out of the corner next to their anchor and overshoot a little; context menus zoom from 96%
    // and slide 4px; dialogs zoom from 95% at the centre.
    const qreal scale = surface_ == DialogSurface ? 0.95 + 0.05 * t : popover ? 0.6 + 0.4 * t : 0.96 + 0.04 * t;
    const qreal offset = surface_ == DialogSurface || popover ? 0 : (1 - t) * 4;
    QPointF origin = QRectF(panel_).center(), shift;
    switch (side_) {
      case Qt::BottomEdge: origin.setY(panel_.top()); shift = QPointF(0, -offset); break;
      case Qt::TopEdge: origin.setY(panel_.bottom()); shift = QPointF(0, offset); break;
      case Qt::RightEdge: origin.setX(panel_.left()); shift = QPointF(-offset, 0); break;
      case Qt::LeftEdge: origin.setX(panel_.right()); shift = QPointF(offset, 0); break;
    }
    if (popover && (side_ == Qt::BottomEdge || side_ == Qt::TopEdge)) {
      const qreal anchorX = QRectF(anchor_).center().x();
      origin.setX(qBound<qreal>(panel_.left(), anchorX, panel_.right()));
    } else if (popover) {
      origin.setY(qBound<qreal>(panel_.top(), QRectF(anchor_).center().y(), panel_.bottom()));
    }
    if (surface_ == DialogSurface) origin = QRectF(panel_).center();
    p.translate(origin + shift);
    p.scale(scale, scale);
    p.translate(-origin);
    // The opacity follows time, not the spring, so the overshoot never flashes.
    const qreal elapsed = fade_.duration() > 0 ? qreal(fade_.currentTime()) / fade_.duration() : 1;
    p.setOpacity(popover && !closing_ ? qMin<qreal>(1, 0.4 + elapsed * 2) : context ? (closing_ ? t : 0.4 + 0.6 * t) : qBound<qreal>(0, t, 1));
  }
  Theme::drawShadow(p, panel_.translated(0, surface_ == PopoverSurface ? 2 : 6), Theme::kRadius2xl, surface_ == PopoverSurface ? 8 : 20, surface_ == PopoverSurface ? 40 : 80);
  if (moving) p.drawPixmap(panel_.topLeft(), frame_);
  else paintSurface(p);
  p.restore();
}
class Menu::Row : public QWidget {
public:
  Row(Menu *menu, int index) : QWidget(menu->content_), menu_(menu), index_(index) {
    setMouseTracking(true);
    const MenuItem &item = menu->items_[index];
    setCursor(item.enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setAccessibleName(item.text);
  }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const MenuItem &item = menu_->items_[index_];
    if (menu_->select_) {
      const QRectF box = QRectF(rect()).adjusted(3, 0, -3, 0);
      if (menu_->hot_ == index_) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 13));
        p.drawRoundedRect(box, Theme::kRadiusXl, Theme::kRadiusXl);
      }
      qreal x = box.x() + 12;
      if (!item.icon.isEmpty()) {
        Theme::drawIcon(p, item.icon, QRectF(x, box.center().y() - 8, 16, 16), Theme::text);
        x += 24;
      }
      p.setPen(Theme::text);
      p.setFont(Theme::font(14));
      p.drawText(QRectF(x, box.y(), box.right() - x - 28, box.height()), Qt::AlignVCenter | Qt::AlignLeft, item.text);
      if (item.checked) Theme::drawIcon(p, "check", QRectF(box.right() - 24, box.center().y() - 8, 16, 16), Theme::muted);
      return;
    }
    const QRectF box = QRectF(rect()).adjusted(3, item.separatorBefore ? 9 : 0, -3, 0);
    if (item.separatorBefore) p.fillRect(QRectF(3, 4, width() - 6, 1), QColor(255, 255, 255, 10));
    if (menu_->hot_ == index_ && item.enabled) {
      p.setPen(Qt::NoPen);
      p.setBrush(item.danger ? QColor(0xfb, 0x2c, 0x36, 26) : Theme::hover);
      p.drawRoundedRect(box, Theme::kRadiusXl, Theme::kRadiusXl);
    }
    QColor ink = item.danger ? Theme::red400 : Theme::text;
    if (!item.enabled) p.setOpacity(0.5);
    qreal x = box.x() + 12 + item.indent * 12;
    if (!item.icon.isEmpty()) {
      Theme::drawIcon(p, item.icon, QRectF(x, box.center().y() - 8, 16, 16), ink);
      x += 24;
    }
    p.setPen(ink);
    p.setFont(Theme::font(14));
    const bool chevron = item.submenu || item.panel || item.chevron;
    const qreal width = box.right() - x - (chevron || item.checked ? 30 : 12);
    p.drawText(QRectF(x, box.y(), width, box.height()), Qt::AlignVCenter | Qt::AlignLeft, Theme::elided(p.font(), item.text, int(width)));
    if (chevron) {
      p.setOpacity(0.6);
      Theme::drawIcon(p, "chevron-right", QRectF(box.right() - 26, box.center().y() - 7, 14, 14), ink);
    }
    if (item.checked) Theme::drawIcon(p, "check", QRectF(box.right() - 26, box.center().y() - 8, 16, 16), Theme::text);
  }
  void mouseMoveEvent(QMouseEvent *) override { menu_->setHot(index_); }
  void leaveEvent(QEvent *) override { if (menu_->hot_ == index_ && !menu_->sub_) menu_->setHot(-1); }
  void mouseReleaseEvent(QMouseEvent *event) override { if (rect().contains(event->position().toPoint())) menu_->trigger(index_); }
private:
  Menu *menu_;
  int index_;
};
Menu::Menu(QWidget *host, QVector<MenuItem> items, int width, bool select) : Popover(host, select ? PopoverSurface : ContextSurface), items_(std::move(items)), select_(select) {
  int y = 3;
  for (int i = 0; i < items_.size(); i++) {
    auto *row = new Row(this, i);
    if (select && items_[i].checked) hot_ = i;
    const int h = select ? 32 : items_[i].separatorBefore ? 45 : 36;
    row->setGeometry(0, y, width - 2, h);
    rows_.push_back(row);
    y += h;
  }
  setPanelSize({width, y + 5});
  content_->setFocusPolicy(Qt::StrongFocus);
  hoverOpen_.setSingleShot(true);
  hoverOpen_.setInterval(140);
  connect(&hoverOpen_, &QTimer::timeout, this, &Menu::hoverSettled);
  connect(this, &Popover::closed, this, [this] {
    if (sub_) sub_->closeNow();
    deleteLater();
  });
}
Menu *Menu::select(QWidget *host, const QVector<MenuItem> &items, const QRect &anchor, Qt::Alignment align, int width) {
  auto *menu = new Menu(host, items, width, true);
  menu->openAt(anchor, Qt::BottomEdge, align);
  return menu;
}
Menu *Menu::popup(QWidget *host, const QVector<MenuItem> &items, const QPoint &at, int width) {
  auto *menu = new Menu(host, items, width);
  menu->openAt(QRect(at, QSize(1, 1)), Qt::RightEdge, Qt::AlignTop);
  return menu;
}
Menu *Menu::popupAt(QWidget *host, const QVector<MenuItem> &items, const QRect &anchor, Qt::Edge side, Qt::Alignment align, int width) {
  auto *menu = new Menu(host, items, width);
  menu->openAt(anchor, side, align);
  return menu;
}
void Menu::setHot(int index) {
  if (hot_ == index) return;
  const int previous = hot_;
  hot_ = index;
  if (previous >= 0 && previous < rows_.size()) rows_[previous]->update();
  if (index >= 0 && index < rows_.size()) rows_[index]->update();
  // With a submenu open, crossing other items on the way to it changes nothing until the pointer rests: the switch
  // waits for the hover delay and is dropped once the pointer has reached the submenu.
  const bool switching = sub_ && subIndex_ != index && index >= 0;
  if (switching || (index >= 0 && (items_[index].submenu || items_[index].panel))) hoverOpen_.start();
  else hoverOpen_.stop();
}
void Menu::hoverSettled() {
  if (sub_ && sub_->underMouse()) {
    // The pointer made it into the submenu: its item stays the highlighted one.
    setHot(subIndex_);
    hoverOpen_.stop();
    return;
  }
  if (hot_ < 0) return;
  if (items_[hot_].submenu || items_[hot_].panel) return openSub(hot_);
  if (sub_) {
    sub_->closeNow();
    sub_ = nullptr;
  }
}
void Menu::openSub(int index) {
  if (index < 0 || index >= items_.size() || !items_[index].enabled) return;
  if (sub_ && subIndex_ == index) return;
  if (sub_) sub_->closeNow();
  const QRect anchor(rows_[index]->mapTo(parentWidget(), QPoint(0, 0)), rows_[index]->size());
  const MenuItem &item = items_[index];
  Popover *next = nullptr;
  if (item.submenu) next = new Menu(parentWidget(), item.submenu(), 240);
  else if (item.panel) next = item.panel(parentWidget());
  if (!next) return;
  next->setKeepWithParent(this);
  sub_ = next;
  subIndex_ = index;
  connect(next, &Popover::closed, this, [this, next] {
    if (sub_ == next) {
      sub_ = nullptr;
      subIndex_ = -1;
      if (isOpen()) content_->setFocus();
    }
  });
  next->openAt(anchor, Qt::RightEdge, Qt::AlignTop);
}
void Menu::trigger(int index) {
  if (index < 0 || index >= items_.size() || !items_[index].enabled) return;
  const MenuItem item = items_[index];
  if (item.submenu || item.panel) return openSub(index);
  closeNow();
  // Close every ancestor menu too before the action can open a new one.
  for (Menu *other : parentWidget()->findChildren<Menu *>(Qt::FindDirectChildrenOnly)) other->closeNow();
  if (item.action) item.action();
}
bool Menu::activate(const QString &text) {
  for (int i = 0; i < items_.size(); i++) {
    if (items_[i].text == text) {
      trigger(i);
      return true;
    }
  }
  return false;
}
bool Menu::eventFilter(QObject *object, QEvent *event) {
  if (event->type() == QEvent::KeyPress && isVisible() && !sub_) {
    auto *key = static_cast<QKeyEvent *>(event);
    int next = hot_;
    const int count = items_.size();
    switch (key->key()) {
      case Qt::Key_Down:
        for (int i = 0; i < count; i++) { next = (next + 1 + count) % count; if (items_[next].enabled) break; }
        setHot(next);
        return true;
      case Qt::Key_Up:
        if (next < 0) next = 0;
        for (int i = 0; i < count; i++) { next = (next - 1 + count) % count; if (items_[next].enabled) break; }
        setHot(next);
        return true;
      case Qt::Key_Right:
        if (hot_ >= 0) openSub(hot_);
        return true;
      case Qt::Key_Left:
        if (hasParentPopover()) { closeNow(); return true; }
        break;
      case Qt::Key_Return:
      case Qt::Key_Enter:
      case Qt::Key_Space:
        if (hot_ >= 0) trigger(hot_);
        return true;
      default: break;
    }
  }
  return Popover::eventFilter(object, event);
}
ToastHost::ToastHost(QWidget *window) : QWidget(window) {
  setAttribute(Qt::WA_TransparentForMouseEvents, false);
  setMouseTracking(true);
  setAttribute(Qt::WA_NoSystemBackground);
  hide();
  spin_.setStartValue(0.0);
  spin_.setEndValue(360.0);
  spin_.setDuration(1000);
  spin_.setLoopCount(-1);
  connect(&spin_, &QVariantAnimation::valueChanged, this, [this] { QWidget::update(); });
}
ToastHost::Toast *ToastHost::find(int id) {
  for (Toast &toast : toasts_)
    if (toast.id == id) return &toast;
  return nullptr;
}
int ToastHost::show(Kind kind, const QString &text, int durationMs) {
  const int id = nextId_++;
  Toast toast{id, kind, text, {}, {}, {}};
  toast.animation = new QVariantAnimation(this);
  toast.animation->setDuration(Theme::motion(200));
  toast.animation->setStartValue(0.0);
  toast.animation->setEndValue(1.0);
  toast.animation->setEasingCurve(Theme::bezier(0.22, 1, 0.36, 1));
  connect(toast.animation, &QVariantAnimation::valueChanged, this, [this, id](const QVariant &v) {
    if (Toast *t = find(id)) {
      t->appear = v.toReal();
      QWidget::update();
    }
  });
  connect(toast.animation, &QVariantAnimation::finished, this, [this, id] {
    if (Toast *t = find(id)) {
      if (t->leaving) remove(id);
      else { t->appear = 1; QWidget::update(); }
    }
  });
  toast.timer = new QTimer(this);
  toast.timer->setSingleShot(true);
  connect(toast.timer, &QTimer::timeout, this, [this, id] { dismiss(id); });
  if (durationMs > 0 && kind != Loading) toast.timer->start(durationMs);
  toasts_.push_back(toast);
  toasts_.back().animation->start();
  relayout();
  QAccessibleEvent announce(this, QAccessible::Alert);
  QAccessible::updateAccessibility(&announce);
  return id;
}
void ToastHost::update(int id, Kind kind, const QString &text, int durationMs) {
  Toast *toast = find(id);
  if (!toast) {
    show(kind, text, durationMs);
    return;
  }
  toast->kind = kind;
  toast->text = text;
  toast->timer->stop();
  if (durationMs > 0 && kind != Loading) toast->timer->start(durationMs);
  relayout();
}
void ToastHost::dismiss(int id) {
  Toast *toast = find(id);
  if (!toast || toast->leaving) return;
  toast->leaving = true;
  toast->animation->stop();
  toast->animation->setDuration(Theme::motion(200));
  toast->animation->setStartValue(toast->appear);
  toast->animation->setEndValue(0.0);
  toast->animation->start();
  if (!toast->animation->duration()) remove(id);
}
void ToastHost::remove(int id) {
  for (int i = 0; i < toasts_.size(); i++) {
    if (toasts_[i].id == id) {
      toasts_[i].animation->deleteLater();
      toasts_[i].timer->deleteLater();
      toasts_.removeAt(i);
      break;
    }
  }
  relayout();
}
int ToastHost::custom(const QString &text, const QByteArray &icon, const QString &action, std::function<void()> onAction) {
  const int id = show(Info, text, 0);
  Toast *toast = find(id);
  toast->icon = icon;
  toast->actionText = action;
  toast->action = std::move(onAction);
  relayout();
  return id;
}
void ToastHost::setProgress(int id, double fraction) {
  if (Toast *toast = find(id)) {
    toast->progress = fraction;
    QWidget::update();
  }
}
void ToastHost::setAction(int id, const QString &action, std::function<void()> onAction) {
  if (Toast *toast = find(id)) {
    toast->actionText = action;
    toast->action = std::move(onAction);
    relayout();
  }
}
void ToastHost::setHiddenForPrivacy(bool hidden) {
  privacy_ = hidden;
  relayout();
}
QStringList ToastHost::texts() const {
  QStringList out;
  for (const Toast &toast : toasts_) out << toast.text;
  return out;
}
void ToastHost::relayout() {
  bool spinning = false;
  for (const Toast &toast : toasts_) spinning |= toast.kind == Loading;
  if (spinning && spin_.state() != QAbstractAnimation::Running && !Theme::reduceMotion()) spin_.start();
  if (!spinning) spin_.stop();
  if (toasts_.isEmpty() || privacy_) {
    hide();
    return;
  }
  QWidget *host = parentWidget();
  const int w = qMin(576, host->width() - 32), h = int(toasts_.size()) * 42 + 36;
  setGeometry((host->width() - w) / 2, host->height() - h - 8, w, h);
  QWidget::show();
  raise();
  QWidget::update();
}
void ToastHost::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QFont textFont = Theme::font(14, QFont::Medium), actionFont = Theme::font(12, QFont::Medium);
  int y = height() - 36;
  for (int i = int(toasts_.size()) - 1; i >= 0; i--) {
    Toast &toast = toasts_[i];
    const int actionWidth = toast.actionText.isEmpty() ? 0 : QFontMetrics(actionFont).horizontalAdvance(toast.actionText) + 24 + 8;
    const int pill = qMin(width(), QFontMetrics(textFont).horizontalAdvance(toast.text) + 14 + 20 + 8 + 14 + actionWidth + 2);
    const QRect box((width() - pill) / 2, y, pill, 36);
    p.save();
    const qreal t = toast.appear;
    p.setOpacity(t);
    p.translate(box.center());
    p.translate(0, (1 - t) * (toast.leaving ? 8 : 16));
    const qreal scale = 0.96 + 0.04 * t;
    p.scale(scale, scale);
    p.translate(-box.center());
    Theme::drawShadow(p, box.translated(0, 4), 18, 12, 77);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Theme::border, 1));
    p.setBrush(Theme::toastFill);
    p.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 18, 18);
    const QRectF iconBox(box.x() + 14, box.center().y() - 9.5, 20, 20);
    switch (toast.kind) {
      case Success: Theme::drawIcon(p, "circle-check", iconBox, Theme::green500); break;
      case Error: Theme::drawIcon(p, "circle-x", iconBox, Theme::red500); break;
      case Info: Theme::drawIcon(p, toast.icon.isEmpty() ? QByteArray("info-circle") : toast.icon, iconBox, toast.icon.isEmpty() ? Theme::blue500 : Theme::text); break;
      case Loading: {
        p.save();
        p.translate(iconBox.center());
        p.rotate(spin_.currentValue().toReal());
        Theme::drawIcon(p, "loader-2", QRectF(-10, -10, 20, 20), Theme::muted);
        p.restore();
      } break;
    }
    int right = box.right() - 14;
    toast.actionRect = {};
    if (!toast.actionText.isEmpty()) {
      p.setFont(actionFont);
      const int w = p.fontMetrics().horizontalAdvance(toast.actionText) + 24;
      toast.actionRect = QRect(right - w + 8, box.center().y() - 13, w, 26);
      p.setPen(Qt::NoPen);
      p.setBrush(Theme::primary);
      p.drawRoundedRect(toast.actionRect, 13, 13);
      p.setPen(Theme::onPrimary);
      p.drawText(toast.actionRect, Qt::AlignCenter, toast.actionText);
      right = toast.actionRect.left() - 8;
    }
    p.setFont(textFont);
    p.setPen(Theme::text);
    const QRect textBox(box.x() + 42, box.y(), right - box.x() - 42, box.height() - (toast.progress >= 0 ? 6 : 0));
    p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, Theme::elided(p.font(), toast.text, textBox.width()));
    if (toast.progress >= 0) {
      const QRectF track(box.x() + 42, box.bottom() - 7, right - box.x() - 42, 3);
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(255, 255, 255, 26));
      p.drawRoundedRect(track, 1.5, 1.5);
      p.setBrush(Theme::primary);
      p.drawRoundedRect(QRectF(track.x(), track.y(), track.width() * qBound(0.0, toast.progress, 1.0), 3), 1.5, 1.5);
    }
    p.restore();
    y -= 42;
  }
}
void ToastHost::mouseMoveEvent(QMouseEvent *event) {
  bool overAction = false;
  for (const Toast &toast : toasts_) overAction |= toast.action && toast.actionRect.contains(event->position().toPoint());
  setCursor(overAction ? Qt::PointingHandCursor : Qt::ArrowCursor);
}
void ToastHost::mousePressEvent(QMouseEvent *event) {
  for (const Toast &toast : toasts_) {
    if (toast.action && toast.actionRect.contains(event->position().toPoint())) {
      const auto action = toast.action;
      action();
      return;
    }
  }
}
// Month and weekday names follow the interface language; a Russian week starts on Monday.
static QLocale pickerLocale() { return QLocale(I18n::language() == QLatin1String("ru") ? QLocale::Russian : QLocale::English); }
static bool weekStartsMonday() { return I18n::language() == QLatin1String("ru"); }
static int firstColumn(const QDate &first) { return weekStartsMonday() ? first.dayOfWeek() - 1 : first.dayOfWeek() % 7; }
static QString monthTitle(int month) {
  const QString name = pickerLocale().standaloneMonthName(month, QLocale::LongFormat);
  return name.left(1).toUpper() + name.mid(1);
}
static constexpr int kPickerWidth = 280, kDaysTop = 84, kCell = 32, kRowStep = 36;
static const qreal kColumnStep = (kPickerWidth - 24) / 7.0 + 4;
DateTimePicker::DateTimePicker(QWidget *parent) : QWidget(parent) {
  setFixedWidth(kPickerWidth);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setAccessibleName(t("contextMenu.changeDate"));
  auto *previous = new IconButton("chevron-left", t("media.previous"), 32, this);
  auto *next = new IconButton("chevron-right", t("media.next"), 32, this);
  previous->move(0, 2);
  next->move(36, 2);
  month_ = new TextButton(QString(), TextButton::Outline, this);
  year_ = new TextButton(QString(), TextButton::Outline, this);
  for (TextButton *select : {month_, year_}) {
    select->setFont(Theme::font(14));
    select->setTrailingIcon("chevron-down");
    select->setAlignLeft(true);
  }
  month_->setGeometry(76, 0, 110, 36);
  year_->setGeometry(194, 0, 86, 36);
  hour_ = new TextField({}, this);
  minute_ = new TextField({}, this);
  hour_->setValidator(new QIntValidator(0, 23, hour_));
  minute_->setValidator(new QIntValidator(0, 59, minute_));
  for (QLineEdit *field : {hour_, minute_}) {
    field->setFixedSize(56, 36);
    field->setFont(Theme::font(16));  // The style sheet's 16px; the font carries that size's tracking.
    field->setAlignment(Qt::AlignCenter);
    field->setAccessibleName(field == hour_ ? QStringLiteral("hour") : QStringLiteral("minute"));
  }
  connect(previous, &QAbstractButton::clicked, this, [this] { const QDate d = value_.date().addMonths(-1); setMonth(d.month(), d.year()); });
  connect(next, &QAbstractButton::clicked, this, [this] { const QDate d = value_.date().addMonths(1); setMonth(d.month(), d.year()); });
  const auto applyTime = [this] {
    value_.setTime(QTime(hour_->text().toInt(), minute_->text().toInt(), value_.time().second()));
    emit edited();
  };
  connect(hour_, &QLineEdit::textEdited, this, applyTime);
  connect(minute_, &QLineEdit::textEdited, this, applyTime);
  connect(month_, &QAbstractButton::clicked, this, [this] {
    QVector<MenuItem> items;
    for (int m = 1; m <= 12; m++) items.push_back({{}, monthTitle(m), [this, m] { setMonth(m, value_.date().year()); }, true, false, false, m == value_.date().month()});
    QWidget *host = window()->findChild<QWidget *>(QStringLiteral("overlayHost"));
    Menu::select(host, items, hostRect(host, month_), Qt::AlignLeft, 124);
  });
  connect(year_, &QAbstractButton::clicked, this, [this] {
    QVector<MenuItem> items;
    const int current = value_.date().year();
    for (int y = current - 7; y <= current + 3; y++) items.push_back({{}, QString::number(y), [this, y] { setMonth(value_.date().month(), y); }, true, false, false, y == current});
    QWidget *host = window()->findChild<QWidget *>(QStringLiteral("overlayHost"));
    Menu::select(host, items, hostRect(host, year_), Qt::AlignLeft, 110);
  });
}
void DateTimePicker::setDateTime(const QDateTime &value) {
  value_ = value;
  sync();
}
int DateTimePicker::rows() const {
  if (!value_.isValid()) return 5;
  const QDate first(value_.date().year(), value_.date().month(), 1);
  return (firstColumn(first) + first.daysInMonth() + 6) / 7;
}
int DateTimePicker::preferredHeight() const { return kDaysTop + rows() * kRowStep - 4 + 12 + 36; }
void DateTimePicker::sync() {
  month_->setText(monthTitle(value_.date().month()));
  year_->setText(QString::number(value_.date().year()));
  hour_->setText(QStringLiteral("%1").arg(value_.time().hour(), 2, 10, QLatin1Char('0')));
  minute_->setText(QStringLiteral("%1").arg(value_.time().minute(), 2, 10, QLatin1Char('0')));
  if (height() != preferredHeight()) {
    setFixedHeight(preferredHeight());
    emit heightChanged();
  }
  QWidget::update();
}
void DateTimePicker::resizeEvent(QResizeEvent *) {
  hour_->move(kPickerWidth / 2 - 9 - 56, height() - 36);
  minute_->move(kPickerWidth / 2 + 9, height() - 36);
}
void DateTimePicker::setMonth(int month, int year) {
  const QDate date(year, month, 1);
  value_.setDate(QDate(year, month, qMin(value_.date().day(), date.daysInMonth())));
  emit edited();
  sync();
}
int DateTimePicker::cellOf(int day) const { return firstColumn(QDate(value_.date().year(), value_.date().month(), 1)) + day - 1; }
QRect DateTimePicker::dayRect(int day) const {
  const int cell = cellOf(day);
  return {qRound(cell % 7 * kColumnStep + (kColumnStep - 4 - kCell) / 2), kDaysTop + cell / 7 * kRowStep, kCell, kCell};
}
int DateTimePicker::dayAt(const QPoint &point) const {
  const int count = value_.date().daysInMonth();
  for (int day = 1; day <= count; day++)
    if (dayRect(day).contains(point)) return day;
  return -1;
}
void DateTimePicker::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setFont(Theme::font(11, QFont::Medium));
  p.setPen(Theme::muted);
  const QLocale locale = pickerLocale();
  for (int d = 0; d < 7; d++) {
    const int weekday = weekStartsMonday() ? d + 1 : (d == 0 ? 7 : d);  // Qt: 1 = Monday … 7 = Sunday.
    QString name = locale.dayName(weekday, QLocale::ShortFormat).left(2);
    name = name.left(1).toUpper() + name.mid(1);
    p.drawText(QRect(qRound(d * kColumnStep), 48, qRound(kColumnStep - 4), 24), Qt::AlignCenter, name);
  }
  p.setFont(Theme::font(14, QFont::Medium));
  const int count = value_.date().daysInMonth();
  for (int day = 1; day <= count; day++) {
    const QRect r = dayRect(day);
    const bool chosen = day == value_.date().day();
    if (chosen || day == hot_) {
      p.setPen(Qt::NoPen);
      p.setBrush(chosen ? QColor(255, 255, 255, 51) : QColor(255, 255, 255, 26));
      p.drawRoundedRect(r, Theme::kRadiusXl, Theme::kRadiusXl);
    }
    p.setPen(Theme::text);
    p.drawText(r, Qt::AlignCenter, QString::number(day));
  }
  p.setFont(Theme::font(14));
  p.setPen(Theme::muted);
  p.drawText(QRect(kPickerWidth / 2 - 9, height() - 36, 18, 36), Qt::AlignCenter, QStringLiteral(":"));
}
void DateTimePicker::mouseMoveEvent(QMouseEvent *event) {
  const int hot = dayAt(event->position().toPoint());
  if (hot != hot_) {
    hot_ = hot;
    setCursor(hot > 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::update();
  }
}
void DateTimePicker::mousePressEvent(QMouseEvent *event) {
  setFocus();
  const int day = dayAt(event->position().toPoint());
  if (day > 0) {
    value_.setDate(QDate(value_.date().year(), value_.date().month(), day));
    QWidget::update();
    emit edited();
  }
}
void DateTimePicker::keyPressEvent(QKeyEvent *event) {
  const int day = event->key() == Qt::Key_Left ? -1 : event->key() == Qt::Key_Right ? 1 : event->key() == Qt::Key_Up ? -7 : event->key() == Qt::Key_Down ? 7 : 0;
  if (day) {
    value_ = value_.addDays(day);
    sync();
    emit edited();
    return;
  }
  QWidget::keyPressEvent(event);
}
