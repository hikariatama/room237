#pragma once
// Small drawn controls. Each one is a real focusable widget with an accessible name and keyboard handling.
#include "theme.h"
#include <QAbstractButton>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTimer>
#include <QVariantAnimation>
#include <QVector>
#include <QWidget>
#include <functional>
// A value that glides to its target and repaints its owner while it moves.
// The focus ring appears for keyboard focus only. One overlay follows the focused control, outside its bounds.
void installFocusRing();
bool focusVisible();
class Glide : public QObject {
public:
  Glide(QWidget *owner, int milliseconds = 120, qreal start = 0);
  void to(qreal target);
  void set(qreal value);
  qreal value() const { return value_; }
  qreal target() const { return target_; }
private:
  QWidget *owner_;
  QVariantAnimation animation_;
  int duration_;
  qreal value_, target_;
};
// A damped spring, as framer-motion's type "spring": stiffness k and damping c for unit mass, stepped in fixed slices.
struct Spring {
  qreal value = 1, velocity = 0, target = 1;
  // Returns whether it is still moving.
  bool step(qreal seconds, qreal stiffness = 600, qreal damping = 25) {
    for (qreal left = seconds; left > 0; left -= 1.0 / 480) {
      const qreal dt = qMin<qreal>(left, 1.0 / 480);
      velocity += (-stiffness * (value - target) - damping * velocity) * dt;
      value += velocity * dt;
    }
    if (qAbs(value - target) < 0.0005 && qAbs(velocity) < 0.005) {
      value = target;
      velocity = 0;
      return false;
    }
    return true;
  }
};
// Scales a widget's painting like framer-motion's whileHover/whileTap (1.1 and 0.9 by default).
class PressScale : public QObject {
public:
  explicit PressScale(QWidget *owner, qreal hover = 1.1, qreal press = 0.9);
  qreal value() const { return spring_.value; }
  void setHovered(bool on) { hovered_ = on; retarget(); }
  void setPressed(bool on) { pressed_ = on; retarget(); }
private:
  void retarget();
  QWidget *owner_;
  qreal hover_, press_;
  bool hovered_ = false, pressed_ = false;
  Spring spring_;
  QTimer tick_;
  qint64 last_ = 0;
};
class IconButton : public QAbstractButton {
  Q_OBJECT
public:
  IconButton(const QByteArray &icon, const QString &label, int side = 32, QWidget *parent = nullptr);
  void setIcon(const QByteArray &icon) { icon_ = icon; update(); }
  void setLabel(const QString &label);
  void setTint(const QColor &color) { tint_ = color; update(); }
  void setIconSize(int size) { iconSize_ = size; update(); }
  void setPermanentFill(bool on) { fill_ = on; update(); }
  void setRadius(int radius) { radius_ = radius; setProperty("ringRadius", radius); update(); }  // Default: round.
  void setColors(const QColor &idle, const QColor &hoverInk, const QColor &hoverFill) { idle_ = idle; hoverInk_ = hoverInk; hoverFill_ = hoverFill; update(); }
  void setBaseFill(const QColor &fill) { base_ = fill; update(); }
  void setHeld(bool held);  // Keeps the hover look while what it opened covers the pointer.
  void setActiveStyle(const QColor &fill, const QColor &border) { activeFill_ = fill; activeBorder_ = border; update(); }
  void setIconAngle(qreal degrees) { angle_ = degrees; update(); }
  // The button grows under the pointer and shrinks while pressed (the viewer's round buttons).
  void setPressScale(bool on);
  QSize sizeHint() const override { return {side_, side_}; }
protected:
  void paintEvent(QPaintEvent *) override;
  void enterEvent(QEnterEvent *) override;
  void leaveEvent(QEvent *) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void changeEvent(QEvent *event) override;
private:
  PressScale *scale_ = nullptr;
  QByteArray icon_;
  int side_, iconSize_;
  Glide hover_;
  QColor tint_, idle_ = Theme::text, hoverInk_ = Theme::text, hoverFill_ = Theme::hover, base_, activeFill_, activeBorder_;
  qreal angle_ = 0;
  bool held_ = false;
  int radius_ = -1;
  bool fill_ = false;
};
class TextButton : public QAbstractButton {
  Q_OBJECT
public:
  enum Kind { Primary, Outline, Ghost, Danger };
  TextButton(const QString &label, Kind kind = Outline, QWidget *parent = nullptr);
  void setIcon(const QByteArray &icon) { icon_ = icon; updateGeometry(); update(); }
  void setTrailingIcon(const QByteArray &icon) { trailing_ = icon; updateGeometry(); update(); }
  void setLeadingPixmap(const QPixmap &pixmap, const QSize &size) { leading_ = pixmap; leadingSize_ = size; updateGeometry(); update(); }
  void setNavigationStyle(bool on) { navigation_ = on; update(); }
  void setDot(bool on) { dot_ = on; update(); }
  void setContentInset(int inset) { inset_ = inset; update(); }
  void setIconInk(const QColor &color) { iconInk_ = color; update(); }
  void setRowColors(const QColor &ink, const QColor &hover) { rowInk_ = ink; rowHover_ = hover; update(); }
  void setBusy(bool busy);
  void setAlignLeft(bool left) { left_ = left; update(); }
  void setSmall(bool small);
  void setRadius(int radius) { radius_ = radius; setProperty("ringRadius", radius); update(); }
  QSize sizeHint() const override;
protected:
  void paintEvent(QPaintEvent *) override;
  void enterEvent(QEnterEvent *) override { hover_.to(1); }
  void leaveEvent(QEvent *) override { hover_.to(0); }
private:
  Kind kind_;
  QByteArray icon_, trailing_;
  QPixmap leading_;
  QSize leadingSize_;
  bool navigation_ = false, dot_ = false;
  int inset_ = -1;
  QColor iconInk_, rowInk_, rowHover_;
  Glide hover_;
  bool left_ = false, busy_ = false, small_ = false;
  int radius_ = Theme::kRadius3xl;
  QVariantAnimation spin_;
};
class Switch : public QAbstractButton {
  Q_OBJECT
public:
  explicit Switch(const QString &label, QWidget *parent = nullptr);
  QSize sizeHint() const override { return {36, 20}; }
protected:
  void paintEvent(QPaintEvent *) override;
  void checkStateSet() override { knob_.to(isChecked() ? 1 : 0); }
  void nextCheckState() override { QAbstractButton::nextCheckState(); knob_.to(isChecked() ? 1 : 0); }
private:
  Glide knob_;
};
class NumberSlider : public QWidget {
  Q_OBJECT
public:
  NumberSlider(int low, int high, int value, const QString &label, QWidget *parent = nullptr);
  int value() const { return value_; }
  void setValue(int value, bool notify = false);
  QSize sizeHint() const override { return {144, 36}; }
signals:
  void valueChanged(int value);
protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
private:
  void fromPosition(qreal x);
  int low_, high_, value_;
  Glide position_;
  qreal wheelAccumulator_ = 0;
};
struct SegmentOption { QByteArray icon; QString label; };
class Segmented : public QWidget {
  Q_OBJECT
public:
  Segmented(const QVector<SegmentOption> &options, const QString &label, QWidget *parent = nullptr);
  int current() const { return current_; }
  void setCurrent(int index, bool notify = false);
  void setLabels(const QString &label, const QStringList &optionLabels);  // Accessible name and the tooltips.
  QSize sizeHint() const override { return {int(options_.size()) * 38, 36}; }
signals:
  void activated(int index);
protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *) override;
  void keyPressEvent(QKeyEvent *event) override;
  bool event(QEvent *event) override;
private:
  QVector<SegmentOption> options_;
  int current_ = 0, hovered_ = -1;
};
class Spinner : public QWidget {
public:
  explicit Spinner(int side = 18, QWidget *parent = nullptr);
protected:
  void paintEvent(QPaintEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;
private:
  QVariantAnimation spin_;
  int side_;
};
void paintProgressRing(QPainter &painter, const QRectF &rect, qreal fraction, const QColor &color, qreal stroke = 2);
// Panel inside the window. Closes on an outside press or Esc and hands focus back.
class Popover : public QWidget {
  Q_OBJECT
public:
  // Popover: fade, zoom from 95%, slide 8, 200ms ease-out. Context: frosted, 200ms.
  // Dialog: dimmed overlay, zoom from 95%, 150ms.
  enum Surface { PopoverSurface, ContextSurface, DialogSurface };
  explicit Popover(QWidget *host, Surface surface = PopoverSurface);
  // anchor is in host coordinates; the panel stays inside the host.
  void openAt(const QRect &anchor, Qt::Edge side = Qt::BottomEdge, Qt::Alignment align = Qt::AlignRight);
  void closeNow();
  void animateClose();
  bool isOpen() const { return open_; }
  void setModalDim(bool dim) { dim_ = dim; }
  void setDismissOnOutside(bool on) { dismissOnOutside_ = on; }
  static void closeAll(QWidget *host);
  void setPanelSize(const QSize &size);
  QRect panelRect() const { return panel_; }
  bool fadeRunning() const { return !frame_.isNull(); }
  void setKeepWithParent(Popover *parent) { parentPopover_ = parent; }
  void setTrigger(QWidget *trigger) { trigger_ = trigger; }  // Focus returns to the trigger on close.
  bool hasParentPopover() const { return parentPopover_ != nullptr; }
signals:
  void closed();
protected:
  void paintEvent(QPaintEvent *) override;
  bool eventFilter(QObject *object, QEvent *event) override;
  void resizeEvent(QResizeEvent *) override;
  QRect contentArea() const;
  virtual void aboutToOpen() {}
  void paintSurface(QPainter &painter);
  QWidget *content_;
  QPointer<QWidget> trigger_;
private:
  void place();
  void startMotion(bool opening);
  QRect panel_, anchor_;
  Qt::Edge side_ = Qt::BottomEdge;
  Qt::Alignment align_ = Qt::AlignRight;
  Surface surface_;
  bool open_ = false, dim_ = false, dismissOnOutside_ = true, closing_ = false;
  QVariantAnimation fade_;
  qreal progress_ = 1;
  QPixmap frame_, frost_;
  QPointer<QWidget> restoreFocus_;
  QPointer<Popover> parentPopover_;
};
struct MenuItem {
  QByteArray icon;
  QString text;
  std::function<void()> action;
  bool enabled = true, danger = false, separatorBefore = false, checked = false;
  std::function<QVector<MenuItem>()> submenu;
  int indent = 0;
  // Custom panel opened to the side instead of a submenu (date picker, confirmations).
  std::function<Popover *(QWidget *host)> panel;
  bool chevron = false;  // A plain row that opens another menu in place.
};
class Menu : public Popover {
  Q_OBJECT
public:
  Menu(QWidget *host, QVector<MenuItem> items, int width = 244, bool select = false);
  // The current row is focused, its check muted.
  static Menu *select(QWidget *host, const QVector<MenuItem> &items, const QRect &anchor, Qt::Alignment align, int width);
  static Menu *popup(QWidget *host, const QVector<MenuItem> &items, const QPoint &at, int width = 244);
  static Menu *popupAt(QWidget *host, const QVector<MenuItem> &items, const QRect &anchor, Qt::Edge side, Qt::Alignment align, int width = 244);
  bool activate(const QString &text);  // QA and accessibility helper.
protected:
  bool eventFilter(QObject *object, QEvent *event) override;
private:
  class Row;
  void openSub(int index);
  void hoverSettled();
  void trigger(int index);
  void setHot(int index);
  QVector<MenuItem> items_;
  QVector<Row *> rows_;
  bool select_ = false;
  int hot_ = -1;
  QPointer<Popover> sub_;
  int subIndex_ = -1;
  QTimer hoverOpen_;
};
class ToastHost : public QWidget {
  Q_OBJECT
public:
  enum Kind { Success, Error, Info, Loading };
  explicit ToastHost(QWidget *window);
  int show(Kind kind, const QString &text, int durationMs = 3200);
  void update(int id, Kind kind, const QString &text, int durationMs = 3200);
  void dismiss(int id);
  // A persistent toast with a progress bar and an action, used by the updater.
  int custom(const QString &text, const QByteArray &icon, const QString &action, std::function<void()> onAction);
  void setProgress(int id, double fraction);
  void setAction(int id, const QString &action, std::function<void()> onAction);
  void setHiddenForPrivacy(bool hidden);
  void hostResized() { if (!toasts_.isEmpty()) relayout(); }
  int visibleCount() const { return toasts_.size(); }
  QStringList texts() const;
protected:
  void paintEvent(QPaintEvent *) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
private:
  struct Toast {
    int id;
    Kind kind;
    QString text, actionText;
    QByteArray icon;
    std::function<void()> action;
    double progress = -1;
    qreal appear = 0;
    QVariantAnimation *animation = nullptr;
    QTimer *timer = nullptr;
    bool leaving = false;
    QRect actionRect;
  };
  void relayout();
  void remove(int id);
  Toast *find(int id);
  QVector<Toast> toasts_;
  int nextId_ = 1;
  QVariantAnimation spin_;
  bool privacy_ = false;
};
// Arrows and month/year selects, Sunday-first day buttons, hour and minute inputs.
class DateTimePicker : public QWidget {
  Q_OBJECT
public:
  explicit DateTimePicker(QWidget *parent = nullptr);
  void setDateTime(const QDateTime &value);
  QDateTime dateTime() const { return value_; }
  int preferredHeight() const;
signals:
  void heightChanged();
  void edited();  // The user changed the date or the time.
protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *) override { hot_ = -1; QWidget::update(); }
  void keyPressEvent(QKeyEvent *event) override;
  void resizeEvent(QResizeEvent *) override;
private:
  int rows() const;
  int cellOf(int day) const;
  QRect dayRect(int day) const;
  int dayAt(const QPoint &point) const;
  void sync();
  void setMonth(int month, int year);
  QDateTime value_;
  int hot_ = -1;
  TextButton *month_, *year_;
  QLineEdit *hour_, *minute_;
};
QPoint hostPoint(QWidget *host, QWidget *from, const QPoint &point);
QRect hostRect(QWidget *host, QWidget *from);
