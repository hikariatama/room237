#include "qa_scroll_log.h"
#include "gallery.h"
#include "sidebar.h"
#include "window.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QScrollBar>
#include <QMouseEvent>
#include <QWheelEvent>
namespace {
class ScrollLog : public QObject {
public:
  ScrollLog(MainWindow *window, const QString &path) : QObject(window), file_(path) {
    file_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered);
    clock_.start();
    watch(QStringLiteral("tree"), window->tree()->verticalScrollBar());
    watch(QStringLiteral("gallery"), window->gallery()->verticalScrollBar());
    qApp->installEventFilter(this);
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (event->type() == QEvent::MouseMove && mouse->buttons() == Qt::NoButton) return false;
      const auto *widget = qobject_cast<QWidget *>(object);
      write(QStringLiteral("%1 to=%2/%3 parent=%4 local=%5,%6 size=%7x%8")
                .arg(QString::fromLatin1(event->type() == QEvent::MouseButtonPress ? "press" : "drag"), QString::fromLatin1(object->metaObject()->className()), object->objectName(),
                     widget && widget->parentWidget() ? QString::fromLatin1(widget->parentWidget()->metaObject()->className()) : QString())
                .arg(qRound(mouse->position().x()))
                .arg(qRound(mouse->position().y()))
                .arg(widget ? widget->width() : 0)
                .arg(widget ? widget->height() : 0));
      return false;
    }
    if (event->type() != QEvent::Wheel) return false;
    const auto *wheel = static_cast<QWheelEvent *>(event);
    static const char *const phases[] = {"none", "begin", "update", "end", "momentum"};
    const int phase = int(wheel->phase());
    write(QStringLiteral("wheel to=%1/%2 phase=%3 pixel=%4,%5 angle=%6,%7 inverted=%8 device=%9 pos=%10,%11")
              .arg(QString::fromLatin1(object->metaObject()->className()), object->objectName(), QString::fromLatin1(phase >= 0 && phase < 5 ? phases[phase] : "?"))
              .arg(wheel->pixelDelta().x())
              .arg(wheel->pixelDelta().y())
              .arg(wheel->angleDelta().x())
              .arg(wheel->angleDelta().y())
              .arg(wheel->inverted() ? 1 : 0)
              .arg(wheel->device() ? int(wheel->device()->type()) : -1)
              .arg(qRound(wheel->globalPosition().x()))
              .arg(qRound(wheel->globalPosition().y())));
    return false;
  }

private:
  void watch(const QString &name, QScrollBar *bar) {
    connect(bar, &QScrollBar::valueChanged, this, [this, name, bar](int value) { write(QStringLiteral("%1 value=%2 max=%3").arg(name).arg(value).arg(bar->maximum())); });
    connect(bar, &QScrollBar::rangeChanged, this, [this, name](int min, int max) { write(QStringLiteral("%1 range=%2..%3").arg(name).arg(min).arg(max)); });
  }
  void write(const QString &line) { file_.write(QByteArray::number(clock_.nsecsElapsed() / 1e6, 'f', 1) + ' ' + line.toUtf8() + '\n'); }
  QFile file_;
  QElapsedTimer clock_;
};
}
void installScrollLog(MainWindow *window, const QString &path) { new ScrollLog(window, path); }
