#include "qa.h"
#include "security.h"
#include "widgets.h"
#include "i18n.h"
#include <QAbstractButton>
#include "platform.h"
#include "qatrace.h"
#include "theme.h"
#include "qa_fence.h"
#ifdef Q_OS_MACOS
#include "qa_native_panel.h"
#endif
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScrollBar>
#include <QWindow>
#include <memory>
#include <QUuid>
#include <QFileDialog>
// Tests name albums by relative path or by folder name.
static const AlbumInfo *qaAlbum(const GalleryModel *model, const QString &key) {
  if (const AlbumInfo *exact = model->albumById(key)) return exact;
  for (const auto &a : model->albums)
    if (a.name == key) return &a;
  return nullptr;
}
QaDriver::QaDriver(MainWindow *window, quint16 port, const QString &fence) : QObject(window), w_(window), fence_(fence) {
  connect(&socket_, &QTcpSocket::connected, this, [this] { socket_.write(QJsonDocument(QJsonObject{{QStringLiteral("event"), QStringLiteral("boot")}}).toJson(QJsonDocument::Compact) + '\n'); });
  connect(&socket_, &QTcpSocket::readyRead, this, [this] {
    buffer_ += socket_.readAll();
    for (;;) {
      const qsizetype end = buffer_.indexOf('\n');
      if (end < 0) break;
      const QJsonObject c = QJsonDocument::fromJson(buffer_.left(end)).object();
      buffer_.remove(0, end + 1);
      handle(c);
    }
  });
  socket_.connectToHost(QHostAddress::LocalHost, port);
  socket_.setSocketOption(QAbstractSocket::LowDelayOption, 1);
  if (gQaTraceOn) {
    qApp->installEventFilter(this);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [](Qt::ApplicationState s) { qaTrace("app-state", QString::number(int(s))); });
    heartbeatClock_.start();
    heartbeat_.setInterval(8);
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
      const double gap = heartbeatClock_.nsecsElapsed() / 1e6;
      heartbeatClock_.start();
      if (gap > 40) qaTrace("loop-gap", QString::number(gap, 'f', 1));
    });
    heartbeat_.start();
  }
}
bool QaDriver::eventFilter(QObject *object, QEvent *event) {
  switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::ShortcutOverride:
      qaTrace("key", QStringLiteral("type=%1 key=%2 mods=%3 to=%4").arg(int(event->type())).arg(static_cast<QKeyEvent *>(event)->key()).arg(int(static_cast<QKeyEvent *>(event)->modifiers())).arg(QString::fromLatin1(object->metaObject()->className())));
      break;
    case QEvent::FocusIn:
    case QEvent::FocusOut:
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
      if (object->isWidgetType()) qaTrace("focus", QStringLiteral("type=%1 to=%2").arg(int(event->type())).arg(QString::fromLatin1(object->metaObject()->className())));
      break;
    default:
      break;
  }
  return false;
}
void QaDriver::reply(const QJsonValue &id, const QJsonValue &result) {
  socket_.write(QJsonDocument(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("ok"), true}, {QStringLiteral("result"), result}}).toJson(QJsonDocument::Compact) + '\n');
}
void QaDriver::fail(const QJsonValue &id, const QString &error) {
  socket_.write(QJsonDocument(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("ok"), false}, {QStringLiteral("error"), error}}).toJson(QJsonDocument::Compact) + '\n');
}
void QaDriver::until(std::function<bool()> test, int timeoutMs, std::function<void(bool)> done) {
  if (test()) {
    done(true);
    return;
  }
  auto *timer = new QTimer(this);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  timer->setInterval(8);
  connect(timer, &QTimer::timeout, this, [=] {
    const bool ok = test();
    if (ok || clock->elapsed() > timeoutMs) {
      timer->stop();
      timer->deleteLater();
      done(ok);
    }
  });
  timer->start();
}
GalleryView::Visible QaDriver::anyVisible(QVector<QPair<QString, bool>> *keys) { return w_->duplicatesVisible() ? w_->duplicates()->visibleState(keys) : w_->gallery()->visibleState(keys); }
QJsonObject QaDriver::state() {
  QJsonArray albums;
  for (const auto &a : w_->model()->albums) albums.push_back(QJsonObject{{QStringLiteral("id"), a.id}, {QStringLiteral("name"), a.name}, {QStringLiteral("parent"), a.parent.isEmpty() ? QJsonValue() : QJsonValue(a.parent)}, {QStringLiteral("size"), a.size}, {QStringLiteral("path"), a.path}});
  return {{QStringLiteral("root"), w_->model()->root}, {QStringLiteral("profile"), w_->profile()}, {QStringLiteral("albumsReady"), w_->albumsReady()}, {QStringLiteral("albums"), albums}, {QStringLiteral("active"), w_->model()->album}, {QStringLiteral("total"), w_->model()->total()}, {QStringLiteral("pageLoading"), w_->model()->busy()}, {QStringLiteral("selection"), w_->gallery()->selectionCount()}, {QStringLiteral("viewer"), w_->viewerIndex()}, {QStringLiteral("duplicates"), w_->duplicatesVisible()}, {QStringLiteral("duplicateTotal"), w_->duplicates()->total()}, {QStringLiteral("hashDone"), w_->hashDone()}, {QStringLiteral("hashTotal"), w_->hashTotal()}, {QStringLiteral("processed"), double(w_->processed())}, {QStringLiteral("focus"), w_->isActiveWindow()}, {QStringLiteral("appActive"), QGuiApplication::applicationState() == Qt::ApplicationActive}, {QStringLiteral("visibility"), w_->isVisible() ? QStringLiteral("visible") : QStringLiteral("hidden")}, {QStringLiteral("coreReady"), w_->coreReady()}, {QStringLiteral("cacheBytes"), double(w_->pipeline()->bytes())}, {QStringLiteral("cacheCount"), w_->pipeline()->count()}, {QStringLiteral("decoded"), double(w_->pipeline()->decoded())}};
}
QJsonObject QaDriver::recordingToJson(FrameRecording rec, double durationMs) {
  QJsonArray times, ys, wheel, samples, latencies;
  for (double t : rec.times) times.push_back(t);
  for (double y : rec.ys) ys.push_back(y);
  for (double t : rec.wheel) wheel.push_back(t);
  for (const auto &s : rec.samples) samples.push_back(QJsonObject{{QStringLiteral("t"), s.t}, {QStringLiteral("visible"), s.visible}, {QStringLiteral("ready"), s.ready}, {QStringLiteral("expected"), s.expected}, {QStringLiteral("blank"), s.blank}, {QStringLiteral("y"), s.y}});
  int censored = 0;
  for (auto it = rec.first.cbegin(); it != rec.first.cend(); ++it) {
    if (!rec.readyAt.contains(it.key())) censored++;
    else latencies.push_back(qRound(rec.readyAt.value(it.key()) - it.value()));
  }
  return {{QStringLiteral("durationMs"), durationMs}, {QStringLiteral("frameTimes"), times}, {QStringLiteral("ys"), ys}, {QStringLiteral("wheelToFrame"), wheel}, {QStringLiteral("samples"), samples}, {QStringLiteral("tileLatencies"), latencies}, {QStringLiteral("censored"), censored}, {QStringLiteral("tilesSeen"), int(rec.first.size())}};
}
void QaDriver::handle(const QJsonObject &c) {
  const QString op = c.value(QLatin1String("op")).toString();
  const QJsonValue id = c.value(QLatin1String("id"));
  auto *clock = new QElapsedTimer;
  clock->start();
  auto ms = [clock] { return clock->nsecsElapsed() / 1e6; };
  auto str = [&c](const char *key) { return c.value(QLatin1String(key)).toString(); };
  if (op == QLatin1String("ping")) {
    reply(id, QJsonObject{{QStringLiteral("variant"), QStringLiteral("qt")}});
  } else if (op == QLatin1String("fonts")) {
    // Which installed font the interface resolved to, and that no font files ride along in the resources.
    QJsonArray bundled, samples;
    for (QDirIterator it(QStringLiteral(":/"), QDirIterator::Subdirectories); it.hasNext();) {
      const QString path = it.next();
      if (path.endsWith(QLatin1String(".ttf"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".otf"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".ttc"), Qt::CaseInsensitive)) bundled.append(path);
    }
    const QString text = c.value(QLatin1String("text")).toString(QStringLiteral("Room237 Albums Альбомы 2024"));
    for (const auto &[px, weight] : {std::pair{10, QFont::Medium}, {12, QFont::Normal}, {14, QFont::Normal}, {14, QFont::DemiBold}, {16, QFont::Medium}, {20, QFont::DemiBold}}) {
      const QFont font = Theme::font(px, weight);
      const QFontInfo info(font);
      const QFontMetricsF metrics(font);
      samples.append(QJsonObject{{QStringLiteral("px"), px}, {QStringLiteral("weight"), int(weight)}, {QStringLiteral("requested"), QJsonArray::fromStringList(font.families())},
                                 {QStringLiteral("family"), info.family()}, {QStringLiteral("style"), info.styleName()}, {QStringLiteral("resolvedWeight"), info.weight()},
                                 {QStringLiteral("pixelSize"), info.pixelSize()}, {QStringLiteral("letterSpacing"), font.letterSpacing()},
                                 {QStringLiteral("advance"), metrics.horizontalAdvance(text)}, {QStringLiteral("height"), metrics.height()}});
    }
    reply(id, QJsonObject{{QStringLiteral("platform"), QGuiApplication::platformName()}, {QStringLiteral("applicationFont"), QFontInfo(QApplication::font()).family()},
                          {QStringLiteral("bundledFontFiles"), bundled}, {QStringLiteral("samples"), samples}});
  } else if (op == QLatin1String("state")) {
    QJsonObject s = state();
    w_->core()->call(QStringLiteral("is_preloading"), {}, [this, id, s](const QJsonValue &v, const QString &) mutable {
      s[QStringLiteral("preloading")] = v;
      reply(id, s);
    });
  } else if (op == QLatin1String("native_directory_open")) {
#ifdef Q_OS_MACOS
    if (nativeDirectoryPending_ || QGuiApplication::platformName() != QLatin1String("cocoa") || !w_->isActiveWindow())
      return fail(id, QStringLiteral("native picker requires already-active owned cocoa window; no activation attempted"));
    const QString kind = str("kind");
    if (kind != QLatin1String("root") && kind != QLatin1String("startup") && kind != QLatin1String("decoy"))
      return fail(id, QStringLiteral("unknown native directory kind"));
    // Optional fenced starting folder: AppKit does not report programmatic navigation of a shown panel.
    const QString start = str("start");
    if (!start.isEmpty() && (!QaFence::allows(start) || !QFileInfo(start).isDir()))
      return fail(id, QStringLiteral("QA refused native dialog start folder"));
    nativeDirectoryPending_ = true;
    nativeDirectorySession_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    reply(id, QJsonObject{{"session", nativeDirectorySession_}});
    const QString session = nativeDirectorySession_;
    QTimer::singleShot(0, this, [this, kind, session, start] {
      // A bind refusal can cancel before the queued picker starts. Never open a cancelled session.
      if (!nativeDirectoryPending_ || nativeDirectorySession_ != session) return;
      w_->pickRootDirectory(kind, start);
      nativeDirectoryPending_ = false;
      nativeDirectorySession_.clear();
      QaNativePanel::retire();
    });
#else
    fail(id, QStringLiteral("native panel adapter is macOS only"));
#endif
  } else if (op == QLatin1String("native_directory_abort")) {
    if (!nativeDirectoryPending_ || str("session").isEmpty() || str("session") != nativeDirectorySession_)
      return fail(id, QStringLiteral("no matching QA-owned directory dialog"));
    if (w_->qaRootDialog_) w_->qaRootDialog_->reject();
    else {
      // This transaction owns only a queued open, not any other window.
      nativeDirectoryPending_ = false;
      nativeDirectorySession_.clear();
    }
    reply(id, QJsonObject{{"cancelled", true}});
  } else if (op == QLatin1String("native_directory_panel")) {
#ifdef Q_OS_MACOS
    if (!nativeDirectoryPending_ || !w_->qaRootDialog_ || str("session").isEmpty() || str("session") != nativeDirectorySession_)
      return fail(id, QStringLiteral("no QA-owned directory dialog with matching session"));
    QString error;
    const QJsonObject result = QaNativePanel::command(w_, str("action"), str("token"), str("expected"), &error);
    if (!error.isEmpty()) return fail(id, error);
    reply(id, result);
#else
    fail(id, QStringLiteral("native panel adapter is macOS only"));
#endif
  } else if (op == QLatin1String("choose_root")) {
    w_->acceptChosenRoot(str("kind"), str("path"), [this, id](const QString &error) {
      if (!error.isEmpty()) return fail(id, error);
      reply(id, state());
    });
  } else if (op == QLatin1String("switch_decoy")) {
    if (!w_->switchDecoy()) return fail(id, QStringLiteral("QA fence refused decoy switch"));
    reply(id, state());
  } else if (op == QLatin1String("clear_decoy")) {
    if (!w_->clearDecoy()) return fail(id, QStringLiteral("QA fence refused decoy clear"));
    reply(id, state());
  } else if (op == QLatin1String("open_root")) {
    if (!w_->openRoot(str("path"))) return fail(id, QStringLiteral("QA fence refused openRoot"));
    reply(id, state());
  } else if (op == QLatin1String("reset_root")) {
    w_->resetRoot();
    reply(id, state());
  } else if (op == QLatin1String("reload_ui")) {
    w_->loadUiState();
    if (w_->uiRootsRefused_) return fail(id, QStringLiteral("QA fence refused saved roots"));
    reply(id, state());
  } else if (op == QLatin1String("save_ui")) {
    w_->saveUiState();
    reply(id, state());
  } else if (op == QLatin1String("dialog_start")) {
    reply(id, w_->rootDialogStart(c.value(QLatin1String("decoy")).toBool()));
  } else if (op == QLatin1String("core_call")) {
    w_->core()->call(str("method"), c.value(QLatin1String("params")).toObject(), [this, id](const QJsonValue &value, const QString &error) {
      if (!error.isEmpty()) return fail(id, error);
      reply(id, value);
    });
  } else if (op == QLatin1String("activate_window")) {
    w_->show();
    w_->raise();
    w_->activateWindow();
    QApplication::setActiveWindow(w_);
    Platform::activateApplication();
    reply(id, QJsonObject{{QStringLiteral("active"), w_->isActiveWindow()}});
  } else if (op == QLatin1String("place")) {
    w_->move(c.value(QLatin1String("x")).toInt(), c.value(QLatin1String("y")).toInt());
    reply(id, QJsonValue());
  } else if (op == QLatin1String("first_visible")) {
    auto *g = w_->gallery();
    const auto [first, last] = g->visibleRange();
    reply(id, QJsonObject{{QStringLiteral("first"), first}, {QStringLiteral("last"), last}, {QStringLiteral("scrollTop"), g->verticalScrollBar()->value()}});
  } else if (op == QLatin1String("focus_state")) {
    QWidget *focus = QApplication::focusWidget();
    reply(id, QJsonObject{{QStringLiteral("widget"), focus ? QString::fromLatin1(focus->metaObject()->className()) : QString()}, {QStringLiteral("visible"), focusVisible()}, {QStringLiteral("treeRow"), w_->tree()->focusRowIndex()}, {QStringLiteral("galleryFocus"), w_->gallery()->focusIndexValue()}});
  } else if (op == QLatin1String("video_fullscreen_exit")) {
    w_->showNormal();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("viewer_wake")) {
    w_->viewer()->wakeForQa();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("video_chrome")) {
    QJsonObject chrome = w_->viewer()->videoChromeState();
    for (QAbstractButton *button : w_->viewer()->findChildren<QAbstractButton *>()) {
      const QString tip = button->toolTip();
      if (tip != t("video.back10") && tip != t("video.forward10")) continue;
      const QPoint p = button->mapToGlobal(QPoint(0, 0));
      chrome[tip == t("video.back10") ? QStringLiteral("back10") : QStringLiteral("forward10")] =
          QJsonObject{{"x", p.x()}, {"y", p.y()}, {"w", button->width()}, {"h", button->height()}};
    }
    reply(id, chrome);
  } else if (op == QLatin1String("dup_tile_rect")) {
    const QRect r = w_->duplicates()->tileGlobalRect(c.value(QLatin1String("group")).toInt(), c.value(QLatin1String("item")).toInt());
    reply(id, QJsonObject{{QStringLiteral("x"), r.x()}, {QStringLiteral("y"), r.y()}, {QStringLiteral("w"), r.width()}, {QStringLiteral("h"), r.height()}});
  } else if (op == QLatin1String("dup_preview_state")) {
    reply(id, w_->duplicates()->previewState());
  } else if (op == QLatin1String("own_window")) {
    // A second top-level window of this process, for the input guard's "own window" case.
    if (c.value(QLatin1String("show")).toBool(true)) {
      if (!ownWindow_) ownWindow_ = new QWidget(nullptr, Qt::Tool);
      ownWindow_->setGeometry(c.value(QLatin1String("x")).toInt(), c.value(QLatin1String("y")).toInt(), 120, 80);
      ownWindow_->show();
    } else if (ownWindow_) {
      ownWindow_->hide();
    }
    reply(id, QJsonValue());
  } else if (op == QLatin1String("resize")) {
    w_->resize(c.value(QLatin1String("w")).toInt(), c.value(QLatin1String("h")).toInt());
    reply(id, QJsonValue());
  } else if (op == QLatin1String("snapshot")) {
    const QString path = str("path");
    if (!QaFence::inside(path, fence_, true)) return fail(id, QStringLiteral("snapshot fence"));
    w_->grab().save(path);
    reply(id, QJsonValue());
  } else if (op == QLatin1String("configure")) {
    if (c.contains(QLatin1String("columns"))) w_->gallery()->setColumns(c.value(QLatin1String("columns")).toInt());
    reply(id, QJsonObject{{QStringLiteral("columns"), w_->gallery()->columns()}});
  } else if (op == QLatin1String("activate")) {
    const AlbumInfo *a = qaAlbum(w_->model(), str("album"));
    if (!a) return fail(id, QStringLiteral("album missing"));
    const QString albumId = a->id;
    const int size = a->size;
    const int timeout = c.value(QLatin1String("timeout")).toInt(60000);
    w_->activateAlbum(albumId, [=, this] {
      const double inventory = ms();
      if (c.value(QLatin1String("noWait")).toBool()) return reply(id, QJsonObject{{QStringLiteral("inventoryMs"), inventory}, {QStringLiteral("total"), w_->model()->total()}});
      until([=, this] { const auto v = w_->gallery()->visibleState(); return size == 0 || (v.expected > 0 && v.ready == v.expected); }, timeout, [=, this](bool ok) {
        if (!ok) return fail(id, QStringLiteral("timeout: visible tiles ready"));
        reply(id, QJsonObject{{QStringLiteral("inventoryMs"), inventory}, {QStringLiteral("readyMs"), ms()}, {QStringLiteral("total"), w_->model()->total()}});
      });
    });
  } else if (op == QLatin1String("wait_ready")) {
    until([this] { const auto v = anyVisible(); return v.expected > 0 && v.ready == v.expected; }, c.value(QLatin1String("timeout")).toInt(30000), [=, this](bool ok) {
      if (!ok) return fail(id, QStringLiteral("timeout: visible tiles ready"));
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("visible"), anyVisible().visible}});
    });
  } else if (op == QLatin1String("visible")) {
    const auto v = anyVisible();
    reply(id, QJsonObject{{QStringLiteral("visible"), v.visible}, {QStringLiteral("ready"), v.ready}, {QStringLiteral("expected"), v.expected}, {QStringLiteral("blank"), v.blank}});
  } else if (op == QLatin1String("scroll_to")) {
    QAbstractScrollArea *area = w_->duplicatesVisible() ? static_cast<QAbstractScrollArea *>(w_->duplicates()) : w_->gallery();
    area->verticalScrollBar()->setValue(qRound(c.value(QLatin1String("y")).toDouble()));
    reply(id, QJsonObject{{QStringLiteral("y"), area->verticalScrollBar()->value()}, {QStringLiteral("max"), area->verticalScrollBar()->maximum()}});
  } else if (op == QLatin1String("geom")) {
    QAbstractScrollArea *area = w_->duplicatesVisible() ? static_cast<QAbstractScrollArea *>(w_->duplicates()) : w_->gallery();
    QWidget *vp = area->viewport();
    const QPoint g = vp->mapToGlobal(QPoint(0, 0));
    const QPoint win = w_->mapToGlobal(QPoint(0, 0));
    reply(id, QJsonObject{{QStringLiteral("window"), QJsonObject{{QStringLiteral("x"), win.x()}, {QStringLiteral("y"), win.y()}, {QStringLiteral("w"), w_->width()}, {QStringLiteral("h"), w_->height()}}}, {QStringLiteral("scale"), w_->devicePixelRatioF()}, {QStringLiteral("viewport"), QJsonObject{{QStringLiteral("x"), g.x()}, {QStringLiteral("y"), g.y()}, {QStringLiteral("w"), vp->width()}, {QStringLiteral("h"), vp->height()}}}, {QStringLiteral("tile"), QJsonObject{{QStringLiteral("w"), w_->gallery()->tilePoints()}, {QStringLiteral("h"), w_->gallery()->tilePoints()}}}, {QStringLiteral("columns"), w_->gallery()->columns()}, {QStringLiteral("scrollTop"), area->verticalScrollBar()->value()}, {QStringLiteral("scrollMax"), area->verticalScrollBar()->maximum()}});
  } else if (op == QLatin1String("tile_rect")) {
    GalleryView *g = w_->gallery();
    const int top = g->verticalScrollBar()->value();
    const double step = g->tilePoints() + (1.0 - g->columns() / 12.0) * 8 + 8;
    const int first = int(std::ceil(top / step)) * g->columns();
    const int index = first + c.value(QLatin1String("index")).toInt();
    const QRect r = g->tileRect(index);
    const QPoint p = g->viewport()->mapToGlobal(r.topLeft());
    const MediaItem *item = w_->model()->item(index);
    reply(id, QJsonObject{{QStringLiteral("x"), p.x()}, {QStringLiteral("y"), p.y()}, {QStringLiteral("w"), r.width()}, {QStringLiteral("h"), r.height()}, {QStringLiteral("name"), item ? item->name : QString()}});
  } else if (op == QLatin1String("find_visible")) {
    GalleryView *g = w_->gallery();
    const double step = g->tilePoints() + (1.0 - g->columns() / 12.0) * 8 + 8;
    const int first = int(g->verticalScrollBar()->value() / step) * g->columns();
    const int last = first + int(g->viewport()->height() / step + 2) * g->columns();
    const QString name = str("name");
    for (int i = first; i < last; i++) {
      const MediaItem *item = w_->model()->item(i);
      if (item && item->name == name) {
        const QRect r = g->tileRect(i);
        const QPoint p = g->viewport()->mapToGlobal(r.topLeft());
        return reply(id, QJsonObject{{QStringLiteral("x"), p.x()}, {QStringLiteral("y"), p.y()}, {QStringLiteral("w"), r.width()}, {QStringLiteral("h"), r.height()}, {QStringLiteral("name"), name}});
      }
    }
    fail(id, QStringLiteral("tile not visible"));
  } else if (op == QLatin1String("album_rect")) {
    const AlbumInfo *a = qaAlbum(w_->model(), str("album"));
    if (!a) return fail(id, QStringLiteral("album missing"));
    const QRect r = w_->tree()->rectFor(a->id);
    const QPoint p = w_->tree()->viewport()->mapToGlobal(r.topLeft());
    reply(id, QJsonObject{{QStringLiteral("x"), p.x()}, {QStringLiteral("y"), p.y()}, {QStringLiteral("w"), r.width()}, {QStringLiteral("h"), r.height()}});
  } else if (op == QLatin1String("record_start")) {
    recordingDup_ = w_->duplicatesVisible();
    recordClock_.start();
    if (recordingDup_) w_->duplicates()->startRecording();
    else w_->gallery()->startRecording();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("record_stop")) {
    FrameRecording rec = recordingDup_ ? w_->duplicates()->stopRecording() : w_->gallery()->stopRecording();
    reply(id, recordingToJson(std::move(rec), recordClock_.nsecsElapsed() / 1e6));
  } else if (op == QLatin1String("selection")) {
    reply(id, QJsonObject{{QStringLiteral("count"), w_->gallery()->selectionCount()}});
  } else if (op == QLatin1String("select_clear")) {
    w_->gallery()->clearSelection();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("select_all")) {
    w_->gallery()->selectAll();
    reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("count"), w_->gallery()->selectionCount()}});
  } else if (op == QLatin1String("wait_selection")) {
    const int want = c.value(QLatin1String("count")).toInt();
    until([=, this] { return w_->gallery()->selectionCount() == want; }, c.value(QLatin1String("timeout")).toInt(10000), [=, this](bool ok) {
      if (!ok) {
        QWidget *focus = QApplication::focusWidget();
        return fail(id, QStringLiteral("timeout: selection count have=%1 focus=%2 active=%3 appState=%4 viewer=%5 modal=%6").arg(w_->gallery()->selectionCount()).arg(focus ? QString::fromLatin1(focus->metaObject()->className()) : QStringLiteral("none")).arg(w_->isActiveWindow()).arg(int(QGuiApplication::applicationState())).arg(w_->viewer()->isOpen()).arg(QApplication::activeModalWidget() != nullptr));
      }
      reply(id, QJsonObject{{QStringLiteral("count"), want}});
    });
  } else if (op == QLatin1String("inventory")) {
    const AlbumInfo *a = qaAlbum(w_->model(), str("album"));
    if (!a) return fail(id, QStringLiteral("album missing"));
    w_->activateAlbum(a->id, [=, this] {
      w_->model()->loadAll([=, this](QVector<MediaItem> items) {
        QStringList names, labels;
        for (const auto &m : items) names << m.name, labels << m.label();
        names.sort();
        labels.sort();
        reply(id, QJsonObject{{QStringLiteral("total"), w_->model()->total()}, {QStringLiteral("names"), QJsonArray::fromStringList(names)}, {QStringLiteral("labels"), QJsonArray::fromStringList(labels)}});
      });
    });
  } else if (op == QLatin1String("prepare_entries")) {
    const AlbumInfo *a = qaAlbum(w_->model(), str("album"));
    if (!a) return fail(id, QStringLiteral("album missing"));
    QSet<QString> want;
    for (const auto &n : c.value(QLatin1String("names")).toArray()) want.insert(n.toString());
    w_->activateAlbum(a->id, [=, this] {
      w_->model()->loadAll([=, this](QVector<MediaItem> items) {
        pendingEntries_.clear();
        for (const auto &m : items)
          if (want.contains(m.name) || want.contains(m.label())) pendingEntries_.push_back(m);
        if (pendingEntries_.size() != want.size()) return fail(id, QStringLiteral("entries mismatch"));
        reply(id, QJsonObject{{QStringLiteral("count"), pendingEntries_.size()}});
      });
    });
  } else if (op == QLatin1String("move_entries")) {
    w_->moveItems(pendingEntries_, str("to"), [=, this](QString error) {
      pendingEntries_.clear();
      if (!error.isEmpty()) return fail(id, error);
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
    });
  } else if (op == QLatin1String("delete_entries")) {
    w_->deleteItems(pendingEntries_, [=, this](QString error) {
      pendingEntries_.clear();
      if (!error.isEmpty()) return fail(id, error);
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
    });
  } else if (op == QLatin1String("import_prepare")) {
    pendingPaths_.clear();
    for (const auto &p : c.value(QLatin1String("paths")).toArray()) {
      if (!QaFence::inside(p.toString(), fence_)) return fail(id, QStringLiteral("import fence"));
      pendingPaths_ << p.toString();
    }
    reply(id, QJsonObject{{QStringLiteral("count"), pendingPaths_.size()}});
  } else if (op == QLatin1String("import_run")) {
    const AlbumInfo *a = qaAlbum(w_->model(), str("album"));
    if (!a) return fail(id, QStringLiteral("album missing"));
    w_->importPaths(pendingPaths_, a->id, [=, this](QString error) {
      pendingPaths_.clear();
      if (!error.isEmpty()) return fail(id, error);
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
    });
  } else if (op == QLatin1String("album_create")) {
    const QString name = str("name");
    const AlbumInfo *parent = c.value(QLatin1String("parent")).isString() ? qaAlbum(w_->model(), str("parent")) : nullptr;
    // "at": an explicit parent directory for the outside-library path. It must already be inside the QA fence.
    const QString at = c.value(QLatin1String("at")).isString() ? str("at") : QString();
    if (!at.isEmpty() && (!QDir::isAbsolutePath(at) || !QaFence::allows(at) || !QFileInfo(at).isDir())) return fail(id, QStringLiteral("album_create at: fenced existing directory required"));
    w_->createAlbum(!at.isEmpty() ? at : parent ? parent->path : w_->model()->root, name, [=, this] { reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("exists"), qaAlbum(w_->model(), name) != nullptr}, {QStringLiteral("scan"), w_->lastAlbumScan()}}); });
  } else if (op == QLatin1String("album_rename")) {
    const AlbumInfo *a = qaAlbum(w_->model(), str("album"));
    if (!a) return fail(id, QStringLiteral("album missing"));
    w_->renameAlbum(a->id, str("name"), [=, this](QString error) {
      if (!error.isEmpty()) return fail(id, error);
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
    });
  } else if (op == QLatin1String("album_move")) {
    const AlbumInfo *a = qaAlbum(w_->model(), str("album"));
    if (!a) return fail(id, QStringLiteral("album missing"));
    const AlbumInfo *parent = c.value(QLatin1String("parent")).isString() ? qaAlbum(w_->model(), str("parent")) : nullptr;
    const QString albumName = a->name;
    w_->moveAlbum(a->id, parent ? parent->id : QString(), [=, this](QString error) {
      if (!error.isEmpty()) return fail(id, error);
      const AlbumInfo *moved = qaAlbum(w_->model(), albumName);
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("parent"), moved && !moved->parent.isEmpty() ? QJsonValue(moved->parent) : QJsonValue()}});
    });
  } else if (op == QLatin1String("album_delete")) {
    const AlbumInfo *a = qaAlbum(w_->model(), str("album"));
    if (!a) return fail(id, QStringLiteral("album missing"));
    w_->deleteAlbum(a->id, [=, this](QString error) {
      if (!error.isEmpty()) return fail(id, error);
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
    });
  } else if (op == QLatin1String("albums_refresh")) {
    // Starts one album listing and answers at once, so a test can keep several in flight.
    w_->model()->refreshAlbums();
    reply(id, QJsonObject{{QStringLiteral("started"), w_->model()->listingsStarted()}, {QStringLiteral("answered"), w_->model()->listingAnswered()}});
  } else if (op == QLatin1String("refresh")) {
    w_->refresh([=, this] {
      until([this] { const auto v = w_->gallery()->visibleState(); return v.expected > 0 && v.ready == v.expected; }, 60000, [=, this](bool ok) {
        if (!ok) return fail(id, QStringLiteral("timeout: refresh ready"));
        reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("total"), w_->model()->total()}});
      });
    });
  } else if (op == QLatin1String("viewer_open")) {
    w_->openViewer(c.value(QLatin1String("index")).toInt());
    until([this] { return w_->viewer()->isOpen() && w_->viewer()->mediaReady(); }, 20000, [=, this](bool ok) {
      if (!ok) return fail(id, QStringLiteral("timeout: viewer"));
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
    });
  } else if (op == QLatin1String("viewer_wait")) {
    const QString previous = str("previous");
    until([=, this] { return w_->viewer()->currentPath() != previous && w_->viewer()->mediaReady(); }, 20000, [=, this](bool ok) {
      if (!ok) return fail(id, QStringLiteral("timeout: viewer step"));
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("path"), w_->viewer()->item().name}});
    });
  } else if (op == QLatin1String("viewer_path")) {
    reply(id, QJsonObject{{QStringLiteral("name"), w_->viewer()->isOpen() ? w_->viewer()->item().name : QString()}, {QStringLiteral("full"), w_->viewer()->currentPath()}, {QStringLiteral("open"), w_->viewer()->isOpen()}, {QStringLiteral("zoom"), w_->viewer()->zoom()}});
  } else if (op == QLatin1String("viewer_step")) {
    w_->stepViewer(c.value(QLatin1String("delta")).toInt());
    reply(id, QJsonValue());
  } else if (op == QLatin1String("viewer_close")) {
    w_->viewer()->closeViewer();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("video_probe")) {
    QMediaPlayer *p = w_->viewer()->player();
    if (!p || !w_->viewer()->isOpen() || !w_->viewer()->isVideo()) return reply(id, QJsonObject{{QStringLiteral("present"), false}});
    reply(id, QJsonObject{{QStringLiteral("present"), true}, {QStringLiteral("currentTime"), p->position() / 1000.0}, {QStringLiteral("paused"), p->playbackState() != QMediaPlayer::PlayingState}, {QStringLiteral("duration"), p->duration() / 1000.0}, {QStringLiteral("total"), double(w_->viewer()->videoFrames())}, {QStringLiteral("dropped"), QJsonValue()}});
  } else if (op == QLatin1String("video_play")) {
    if (QMediaPlayer *p = w_->viewer()->player()) p->play();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("video_pause")) {
    if (QMediaPlayer *p = w_->viewer()->player()) p->pause();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("dup_open")) {
    w_->showDuplicates(true, [=, this] {
      const double firstGroup = ms();
      until([this] { const auto v = w_->duplicates()->visibleState(); return v.visible > 0 && v.ready == v.expected; }, 60000, [=, this](bool ok) {
        if (!ok) return fail(id, QStringLiteral("timeout: duplicates ready"));
        reply(id, QJsonObject{{QStringLiteral("firstGroupMs"), firstGroup}, {QStringLiteral("readyMs"), ms()}, {QStringLiteral("groups"), w_->duplicates()->total()}, {QStringLiteral("files"), w_->duplicates()->filesTotal()}});
      });
    });
  } else if (op == QLatin1String("dup_close")) {
    w_->showDuplicates(false);
    reply(id, QJsonValue());
  } else if (op == QLatin1String("dup_state")) {
    reply(id, QJsonObject{{QStringLiteral("total"), w_->duplicates()->total()}, {QStringLiteral("files"), w_->duplicates()->filesTotal()}, {QStringLiteral("loading"), w_->duplicates()->loading()}});
  } else if (op == QLatin1String("dup_ignore")) {
    w_->duplicates()->markDifferent(c.value(QLatin1String("group")).toInt(), [=, this] { reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("groups"), w_->duplicates()->total()}}); });
  } else if (op == QLatin1String("dup_keep")) {
    w_->duplicates()->keepFirst(c.value(QLatin1String("group")).toInt(), [=, this](QString kept) { reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("kept"), kept}, {QStringLiteral("groups"), w_->duplicates()->total()}}); });
  } else if (op == QLatin1String("dup_delete_all")) {
    w_->duplicates()->deleteAll([=, this](int deleted) { reply(id, QJsonObject{{QStringLiteral("ms"), ms()}, {QStringLiteral("deleted"), deleted}}); });
  } else if (op == QLatin1String("menu_state")) {
    bool open = false;
    for (Popover *popover : w_->overlayHost()->findChildren<Popover *>(Qt::FindDirectChildrenOnly)) open |= popover->isOpen();
    reply(id, QJsonObject{{QStringLiteral("open"), open}});
  } else if (op == QLatin1String("core")) {
    w_->core()->call(str("method"), c.value(QLatin1String("params")).toObject(), [=, this](const QJsonValue &result, const QString &error) {
      if (!error.isEmpty()) return fail(id, error);
      reply(id, result);
    });
  } else if (op == QLatin1String("apply_settings")) {
    w_->core()->call(QStringLiteral("update_settings"), {{QStringLiteral("settings"), c.value(QLatin1String("settings")).toObject()}}, [=, this](const QJsonValue &result, const QString &error) {
      if (!error.isEmpty()) return fail(id, error);
      w_->applySettingsForQa(result.isObject() ? result.toObject() : c.value(QLatin1String("settings")).toObject());
      reply(id, result);
    });
  } else if (op == QLatin1String("set_columns")) {
    w_->setColumnsForQa(c.value(QLatin1String("columns")).toInt());
    reply(id, QJsonValue());
  } else if (op == QLatin1String("set_layout")) {
    w_->setLayoutKind(str("layout"));
    reply(id, QJsonObject{{QStringLiteral("layout"), w_->ui().layout}});
  } else if (op == QLatin1String("toasts")) {
    reply(id, QJsonObject{{QStringLiteral("texts"), QJsonArray::fromStringList(w_->toasts()->texts())}});
  } else if (op == QLatin1String("viewer_state")) {
    Viewer *v = w_->viewer();
    reply(id, QJsonObject{{QStringLiteral("open"), v->isOpen()}, {QStringLiteral("ready"), v->mediaReady()}, {QStringLiteral("failed"), v->failed()}, {QStringLiteral("animated"), v->isAnimated()}, {QStringLiteral("animationFrames"), double(v->animationFrames())}, {QStringLiteral("videoFrames"), double(v->videoFrames())}, {QStringLiteral("zoom"), v->zoom()}, {QStringLiteral("suspended"), v->suspended()}, {QStringLiteral("path"), v->currentPath()}});
  } else if (op == QLatin1String("viewer_zoom")) {
    w_->viewer()->setZoom(c.value(QLatin1String("zoom")).toDouble());
    reply(id, QJsonObject{{QStringLiteral("zoom"), w_->viewer()->zoom()}});
  } else if (op == QLatin1String("privacy_state")) {
    reply(id, QJsonObject{{QStringLiteral("locked"), w_->locked()}, {QStringLiteral("shielded"), w_->shielded()}, {QStringLiteral("decoy"), w_->displayDecoy()}, {QStringLiteral("privacy"), w_->privacyEnabled()}, {QStringLiteral("lockscreen"), w_->lockscreenEnabled()}, {QStringLiteral("root"), w_->model()->root}});
  } else if (op == QLatin1String("vault_state")) {
    QJsonObject out = w_->vaultScreen()->qaState();
    out.insert(QStringLiteral("protected"), w_->inProtectedRoot());
    out.insert(QStringLiteral("root"), w_->model()->root);
    out.insert(QStringLiteral("decoy"), w_->displayDecoy());
    out.insert(QStringLiteral("unlockRoot"), w_->vaultScreen()->root());
    QJsonArray panels;
    for (Popover *panel : w_->overlayHost()->findChildren<Popover *>())
      if (panel->isOpen() && !panel->objectName().isEmpty()) panels.append(panel->objectName());
    out.insert(QStringLiteral("panels"), panels);
    reply(id, out);
  } else if (op == QLatin1String("vault_unlock_ui")) {
    w_->vaultScreen()->qaSubmit(str("password"), str("recoveryKey"), c.value(QLatin1String("recovery")).toBool(), c.value(QLatin1String("secondary")).toBool());
    until([this] { return !w_->vaultScreen()->qaState().value(QLatin1String("busy")).toBool(); }, 30000, [=, this](bool ok) {
      if (!ok) return fail(id, QStringLiteral("unlock did not finish"));
      reply(id, w_->vaultScreen()->qaState());
    });
  } else if (op == QLatin1String("encrypt_offer")) {
    auto *offer = w_->overlayHost()->findChild<EncryptOffer *>();
    if (!offer || !offer->isOpen()) return fail(id, QStringLiteral("no encryption offer"));
    offer->qaChoose(str("choice"));
    reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
  } else if (op == QLatin1String("hardening_setup")) {
    const auto find = [this]() -> HardeningSetup * {
      for (auto *setup : w_->overlayHost()->findChildren<HardeningSetup *>())
        if (setup->isOpen()) return setup;
      return nullptr;
    };
    until([find] { return find() && find()->qaFactorsLoaded(); }, 5000, [=, this](bool open) {
      if (!open) return fail(id, QStringLiteral("setup is not open"));
      QStringList kinds;
      for (const QJsonValue &kind : c.value(QLatin1String("kinds")).toArray()) kinds << kind.toString();
      const QString password = str("password");
      find()->qaSubmit(password, c.contains(QLatin1String("repeat")) ? str("repeat") : password, str("decoy"), kinds);
      until([find] { return !find() || !find()->qaRecoveryKey().isEmpty() || !find()->qaError().isEmpty(); }, 30000, [=, this](bool ok) {
        HardeningSetup *setup = find();
        if (!ok || !setup) return fail(id, QStringLiteral("setup did not finish"));
        QJsonObject out{{QStringLiteral("recoveryKey"), setup->qaRecoveryKey()}, {QStringLiteral("error"), setup->qaError()}, {QStringLiteral("blurred"), setup->qaKeyBlurred()}};
        if (!setup->qaRecoveryKey().isEmpty()) {
          // A wrong check first when asked for one, then the right groups unless the dialog is to stay open.
          QStringList wrong;
          for (const QJsonValue &group : c.value(QLatin1String("wrongCheck")).toArray()) wrong << group.toString();
          if (!wrong.isEmpty()) out.insert(QStringLiteral("wrongCheckError"), setup->qaDone(wrong));
          if (c.value(QLatin1String("finish")).toBool(true)) out.insert(QStringLiteral("checkError"), setup->qaDone());
        }
        reply(id, out);
      });
    });
  } else if (op == QLatin1String("processing_state")) {
    PreloadOverlay *screen = w_->processingScreen();
    QWidget *host = w_->overlayHost();
    // What a click on the sidebar and on the gallery would reach.
    QJsonArray hits;
    for (const QPoint point : {QPoint(60, 100), QPoint(host->width() * 3 / 4, host->height() - 60)}) {
      QWidget *hit = host->childAt(point);
      hits.append(hit && (hit == screen || screen->isAncestorOf(hit)));
    }
    int enabled = 0;
    for (QAction *action : w_->actions()) enabled += action->isEnabled();
    reply(id, QJsonObject{{QStringLiteral("visible"), screen->isVisible()}, {QStringLiteral("vault"), screen->qaVault()}, {QStringLiteral("blocksClicks"), hits}, {QStringLiteral("enabledActions"), enabled}, {QStringLiteral("frosted"), screen->qaFrosted()}});
  } else if (op == QLatin1String("processing_workers")) {
    PreloadOverlay *screen = w_->processingScreen();
    screen->qaSetWorkers(c.value(QLatin1String("thumbnails")).toInt(), c.value(QLatin1String("metadata")).toInt(), c.value(QLatin1String("encryption")).toInt(4));
    reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
  } else if (op == QLatin1String("setup_page")) {
    for (auto *setup : w_->overlayHost()->findChildren<HardeningSetup *>())
      if (setup->isOpen()) {
        if (str("page") == QLatin1String("reveal")) setup->qaReveal();
        else setup->qaShowCheck();
        return reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
      }
    fail(id, QStringLiteral("setup is not open"));
  } else if (op == QLatin1String("export_confirm")) {
    for (auto *panel : w_->overlayHost()->findChildren<ConfirmPanel *>())
      if (panel->isOpen() && panel->objectName() == QLatin1String("exportConfirm")) {
        for (auto *button : panel->findChildren<TextButton *>())
          if (button->text() == t("security.export.action")) button->click();
        return reply(id, QJsonObject{{QStringLiteral("confirmed"), true}});
      }
    fail(id, QStringLiteral("no export confirmation"));
  } else if (op == QLatin1String("set_locked")) {
    w_->setLocked(c.value(QLatin1String("locked")).toBool());
    reply(id, QJsonObject{{QStringLiteral("locked"), w_->locked()}});
  } else if (op == QLatin1String("set_unfocused")) {
    w_->setUnfocusedForQa(c.value(QLatin1String("unfocused")).toBool());
    reply(id, QJsonObject{{QStringLiteral("shielded"), w_->shielded()}});
  } else if (op == QLatin1String("favorite")) {
    const MediaItem *item = w_->model()->item(c.value(QLatin1String("index")).toInt());
    if (!item) return fail(id, QStringLiteral("item missing"));
    const bool on = !item->favorite;
    w_->setFavorite(*item, on, [=, this](QString error) {
      if (!error.isEmpty()) return fail(id, error);
      reply(id, QJsonObject{{QStringLiteral("favorite"), on}});
    });
  } else if (op == QLatin1String("set_date")) {
    const MediaItem *item = w_->model()->item(c.value(QLatin1String("index")).toInt());
    if (!item) return fail(id, QStringLiteral("item missing"));
    w_->setItemsDate({*item}, qint64(c.value(QLatin1String("timestamp")).toDouble()), [=, this](QString error) {
      if (!error.isEmpty()) return fail(id, error);
      reply(id, QJsonValue());
    });
  } else if (op == QLatin1String("set_sort")) {
    w_->setSort(str("key"));
    if (c.contains(QLatin1String("dir"))) w_->setSortDirection(str("dir"));
    reply(id, QJsonValue());
  } else if (op == QLatin1String("item")) {
    const MediaItem *item = w_->model()->item(c.value(QLatin1String("index")).toInt());
    if (!item) return fail(id, QStringLiteral("item missing"));
    reply(id, QJsonObject{{QStringLiteral("name"), item->name}, {QStringLiteral("label"), item->label()}, {QStringLiteral("favorite"), item->favorite}, {QStringLiteral("video"), item->isVideo}, {QStringLiteral("width"), item->width}, {QStringLiteral("height"), item->height}, {QStringLiteral("dateTaken"), item->dateTaken}, {QStringLiteral("dateAdded"), item->dateAdded}, {QStringLiteral("revision"), item->revision}, {QStringLiteral("album"), item->albumId}});
  } else if (op == QLatin1String("copy_selection")) {
    w_->gallery()->collectSelection([=, this](const QVector<MediaItem> &items) {
      if (qEnvironmentVariableIntValue("ROOM237_QA_PRIVATE_CLIPBOARD") == 1) {
        QStringList paths;
        for (const MediaItem &item : items) {
          if (!QaFence::inside(item.path(), fence_)) return fail(id, QStringLiteral("private clipboard fence"));
          paths << item.path();
        }
        setProperty("qaPrivateClipboard", paths);
      } else w_->copyItems(items);
      reply(id, QJsonObject{{QStringLiteral("count"), items.size()}});
    });
  } else if (op == QLatin1String("clipboard")) {
    if (qEnvironmentVariableIntValue("ROOM237_QA_PRIVATE_CLIPBOARD") == 1) {
      reply(id, QJsonObject{{"urls", QJsonArray::fromStringList(property("qaPrivateClipboard").toStringList())}, {"text", QString()}, {"hasImage", false}});
      return;
    }
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    QJsonArray urls;
    if (mime) for (const QUrl &url : mime->urls()) urls.push_back(url.toLocalFile());
    reply(id, QJsonObject{{QStringLiteral("urls"), urls}, {QStringLiteral("text"), mime ? mime->text() : QString()}, {QStringLiteral("hasImage"), mime && mime->hasImage()}});
  } else if (op == QLatin1String("paste")) {
    if (qEnvironmentVariableIntValue("ROOM237_QA_PRIVATE_CLIPBOARD") == 1) {
      const QStringList paths = property("qaPrivateClipboard").toStringList();
      for (const QString &path : paths) if (!QaFence::inside(path, fence_)) return fail(id, QStringLiteral("private clipboard fence"));
      w_->importPaths(paths, w_->model()->album, [=, this](QString error) {
        if (!error.isEmpty()) return fail(id, error);
        reply(id, QJsonValue());
      });
      return;
    }
    w_->pasteFromClipboard();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("update_check")) {
    w_->updater()->checkNow(true);
    reply(id, QJsonValue());
  } else if (op == QLatin1String("update_install")) {
    w_->updater()->install();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("update_cancel")) {
    w_->updater()->cancel();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("update_state")) {
    reply(id, QJsonObject{{QStringLiteral("state"), w_->updater()->state()}, {QStringLiteral("version"), w_->updater()->availableVersion()}, {QStringLiteral("current"), QStringLiteral(ROOM237_VERSION)}, {QStringLiteral("result"), w_->updater()->previousResult()}, {QStringLiteral("fixtureAllowed"), Updater::qaFixtureAllowed()}, {QStringLiteral("pid"), double(QCoreApplication::applicationPid())}});
  } else if (op == QLatin1String("language")) {
    w_->applyLanguage(str("language"));
    reply(id, QJsonValue());
  } else if (op == QLatin1String("open_advanced")) {
    w_->openAdvancedSettings();
    reply(id, QJsonValue());
  } else if (op == QLatin1String("album_favorites")) {
    w_->activateAlbum(kFavoritesId, [=, this] { reply(id, QJsonObject{{QStringLiteral("total"), w_->model()->total()}}); });
  } else if (op == QLatin1String("settings_open")) {
    QTimer::singleShot(0, w_, [this] { w_->openSettings(); });
    until([this] { return w_->overlayHost()->findChild<SettingsPanel *>() != nullptr; }, 5000, [=, this](bool ok) {
      if (!ok) return fail(id, QStringLiteral("settings did not open"));
      reply(id, QJsonObject{{QStringLiteral("ms"), ms()}});
    });
  } else if (op == QLatin1String("settings_close")) {
    Popover::closeAll(w_->overlayHost());
    reply(id, QJsonObject{{QStringLiteral("open"), false}});
  } else if (op == QLatin1String("quit_now")) {
    reply(id, QJsonValue());
    socket_.flush();
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
  } else if (op == QLatin1String("set_cache_budget")) {
    w_->pipeline()->setBudget(qint64(c.value(QLatin1String("bytes")).toDouble()));
    reply(id, QJsonValue());
  } else {
    fail(id, QStringLiteral("unknown op ") + op);
  }
}
