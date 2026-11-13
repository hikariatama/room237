#include "window.h"
#include "i18n.h"
#include "platform.h"
#include "qatrace.h"
#include "qa_fence.h"
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QMap>
#include <QPointer>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QSignalBlocker>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include "secure_media.h"
#include "vault_ops.h"
static const QStringList kMediaExtensions = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("webp"), QStringLiteral("avif"), QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("flv"), QStringLiteral("m4v"), QStringLiteral("ogg"), QStringLiteral("ogv"), QStringLiteral("wmv"), QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("3gp"), QStringLiteral("mts"), QStringLiteral("m2ts")};
static bool isMediaFile(const QString &path) { return kMediaExtensions.contains(QFileInfo(path).suffix().toLower()); }
// A shortcut works on a Russian layout too: the physical key stays the same.
static bool keyIs(QKeyEvent *event, QChar latin) {
  static const QHash<QChar, QChar> cyrillic{{QLatin1Char('l'), QChar(0x0434)}, {QLatin1Char('u'), QChar(0x0433)}, {QLatin1Char('a'), QChar(0x0444)}, {QLatin1Char('c'), QChar(0x0441)}, {QLatin1Char('d'), QChar(0x0432)}, {QLatin1Char('v'), QChar(0x043C)}};
  if (event->key() == latin.toUpper().unicode()) return true;
  const QString text = event->text().toLower();
  return !text.isEmpty() && (text[0] == latin || text[0] == cyrillic.value(latin));
}
// The window background: bg.png tiled over the page colour.
class PageWidget : public QWidget {
public:
  using QWidget::QWidget;
protected:
  void paintEvent(QPaintEvent *event) override {
    QPainter p(this);
    p.setClipRegion(event->region());
    Theme::paintPage(p, rect(), QPoint(0, 0));
  }
};
class SidebarCard : public QWidget {
public:
  using QWidget::QWidget;
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Theme::border, 1));
    p.setBrush(Theme::sidebar);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRadius2xl, Theme::kRadius2xl);
  }
};
class HashPill : public QWidget {
public:
  using QWidget::QWidget;
  void setProgress(const QString &text, qreal fraction) {
    text_ = text;
    fraction_ = fraction;
    setMaximumWidth(sizeHint().width());
    updateGeometry();
    update();
  }
  QSize sizeHint() const override { return {8 + 14 + 6 + QFontMetrics(Theme::font(12)).horizontalAdvance(text_) + 10, 24}; }
  QSize minimumSizeHint() const override { return {28, 24}; }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 13));
    p.drawRoundedRect(rect(), height() / 2.0, height() / 2.0);
    constexpr qreal unit = 14.0 / 36, radius = 15.9155 * unit;
    const QRectF ring(8 + 7 - radius, height() / 2.0 - radius, 2 * radius, 2 * radius);
    p.setPen(QPen(Theme::secondary, 5 * unit));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(ring);
    p.setPen(QPen(Theme::primary, 5 * unit, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(ring, 90 * 16, -int(fraction_ * 360 * 16));
    p.setPen(Theme::muted);
    p.setFont(Theme::font(12));
    p.drawText(rect().adjusted(28, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(text_, Qt::ElideRight, width() - 38));
  }
private:
  QString text_;
  qreal fraction_ = 0;
};
MainWindow::MainWindow(const QString &profile, const QString &root, const QString &legacyProfile) : model_(&core_), pipeline_(4, envMegabytes("ROOM237_CACHE_MB", 96)), profile_(profile), cliRoot_(root) {
  setWindowTitle(QStringLiteral("Room237"));
  setMinimumSize(1000, 450);
  loadUiState();
  I18n::setLanguage(ui_.language);
  host_ = new PageWidget;
  host_->setObjectName(QStringLiteral("overlayHost"));
  setCentralWidget(host_);
  // The content runs under the transparent title bar on macOS; the sidebar leaves room for the traffic lights.
  setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
  host_->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
  buildContent();
  buildActions();
  // The native window already exists here, and changing its style mask reports its old frame back.
  if (QGuiApplication::platformName() != QLatin1String("offscreen")) Platform::configureWindow(this);
  resize(1280, 720);
  setFfmpegPath(QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
      QStringLiteral("ffmpeg.exe")
#else
      QStringLiteral("ffmpeg")
#endif
          ));
  connect(&core_, &CoreClient::event, this, &MainWindow::onEvent);
  connect(&core_, &CoreClient::disconnected, this, [this](const QString &message) {
    if (closing_) return;
    if (!coreReady_) {
      // The core refuses corrupt settings on purpose: silently using defaults could turn privacy off.
      startupScreen_->setError(t("settings.loadFailed"), core_.lastError().isEmpty() ? message : core_.lastError());
      return;
    }
    showToast(1, message);
  });
  reconcileTimer_.setSingleShot(true);
  reconcileTimer_.setInterval(150);
  connect(&reconcileTimer_, &QTimer::timeout, this, &MainWindow::reconcileNow);
  reconcileClock_.start();
  connect(&watcher_, &RootWatcher::changed, this, &MainWindow::scheduleReconcile);
  thumbRefresh_.setSingleShot(true);
  thumbRefresh_.setInterval(1000);
  connect(&thumbRefresh_, &QTimer::timeout, this, [this] {
    pipeline_.forgetMissing();
    // A query by a name that has just moved away reads nothing; followActiveAlbum is about to sort that out.
    const AlbumInfo *open = model_.albumById(model_.album);
    if (open && !QFileInfo::exists(open->path)) return;
    model_.refreshInPlace();
  });
  saveTimer_.setSingleShot(true);
  saveTimer_.setInterval(300);
  connect(&saveTimer_, &QTimer::timeout, this, &MainWindow::saveUiState);
  inactivity_.setSingleShot(true);
  inactivity_.setInterval(5 * 60 * 1000);
  connect(&inactivity_, &QTimer::timeout, this, [this] {
    if ((lockscreenEnabled_ || inProtectedRoot()) && !locked_) setLocked(true);
  });
  windowStateTimer_.setSingleShot(true);
  windowStateTimer_.setInterval(0);
  connect(&windowStateTimer_, &QTimer::timeout, this, [this] {
    viewer_->setSuspended(locked_ || unfocused_ || isMinimized() || !isVisible());
  });
  updater_ = new Updater(&core_, profile_, this);
  updater_->setAutomatic(ui_.autoUpdate && !QaFence::enabled());
  connect(updater_, &Updater::message, this, [this](int kind, const QString &text) { showToast(kind, text); });
  connect(updater_, &Updater::available, this, [this](const QString &version) {
    if (updateToast_) toasts_->dismiss(updateToast_);
    updateToast_ = toasts_->custom(t("update.available", {{"version", version}}), "cloud-download", t("update.install"), [this] { updater_->install(); });
  });
  connect(updater_, &Updater::manualOnly, this, [this](const QString &reason, const QString &version) {
    if (updateToast_) toasts_->dismiss(updateToast_);
    updateToast_ = toasts_->custom(reason == QLatin1String("translocated") ? t("update.translocated") : t("update.available", {{"version", version}}) + QStringLiteral(". ") + t("update.manual"), "cloud-download", reason == QLatin1String("translocated") ? QString() : t("update.openPage"), [] {
      QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/hikariatama/room237/releases/latest")));
    });
  });
  connect(updater_, &Updater::downloading, this, [this] {
    toasts_->update(updateToast_, ToastHost::Info, t("update.downloading"), 0);
    toasts_->setAction(updateToast_, t("update.cancel"), [this] { updater_->cancel(); });
    toasts_->setProgress(updateToast_, 0);
  });
  connect(updater_, &Updater::progress, this, [this](double fraction) { toasts_->setProgress(updateToast_, fraction); });
  connect(updater_, &Updater::failed, this, [this](const QString &text) {
    if (updateToast_) toasts_->dismiss(updateToast_);
    updateToast_ = toasts_->custom(text, "alert-triangle", t("update.retry"), [this] { updater_->install(); });
  });
  connect(updater_, &Updater::readyToRestart, this, [this] {
    closing_ = true;
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
  });
  qApp->installEventFilter(this);
  QStringList arguments{QStringLiteral("--profile"), profile};
  if (!legacyProfile.isEmpty() && QFileInfo(legacyProfile).isDir()) arguments << QStringLiteral("--legacy-profile") << legacyProfile;
  core_.start(arguments);
  // A migrated state file appears after the core started; read it once more when the core is up.
  core_.call(QStringLiteral("retry_startup"));
  startupScreen_->setBusy(t("startup.checking"));
}
MainWindow::~MainWindow() {
  closing_ = true;
  saveUiState();
}
void MainWindow::buildContent() {
  // Room for the traffic lights above the tree; a bottom row under a divider.
  sidebarPanel_ = new QWidget(host_);
  sidebar_ = new SidebarCard(sidebarPanel_);
  sidebar_->setObjectName(QStringLiteral("sidebar"));
  auto *sideLayout = new QVBoxLayout(sidebar_);
  sideLayout->setContentsMargins(9, 41, 9, 9);
  sideLayout->setSpacing(0);
  tree_ = new AlbumTree;
  sideLayout->addWidget(tree_, 1);
  sideLayout->addSpacing(6);
  auto *rule = new QWidget;
  rule->setFixedHeight(1);
  rule->setStyleSheet(QStringLiteral("background:rgba(255,255,255,15)"));
  sideLayout->addWidget(rule);
  sideLayout->addSpacing(6);
  auto *bottom = new QHBoxLayout;
  bottom->setContentsMargins(0, 0, 0, 0);
  bottom->setSpacing(4);
  keyboardButton_ = new IconButton("keyboard", t("hotkeys.show"), 32);
  settingsButton_ = new IconButton("settings", t("settings.title"), 32);
  hashPill_ = new HashPill;
  hashPill_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  hashPill_->setFixedHeight(24);
  hashPill_->hide();
  bottom->addWidget(keyboardButton_);
  bottom->addWidget(hashPill_, 1);
  bottom->addStretch();
  bottom->addWidget(settingsButton_);
  sideLayout->addLayout(bottom);
  gearSpin_.setDuration(300);
  connect(&gearSpin_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { settingsButton_->setIconAngle(v.toReal()); });
  rightPane_ = new QWidget(host_);
  auto *rightLayout = new QVBoxLayout(rightPane_);
  rightLayout->setContentsMargins(0, 0, 0, 8);  // The gallery ends level with the sidebar card.
  rightLayout->setSpacing(0);
  header_ = new QWidget;
  header_->setFixedHeight(54);
  auto *bar = new QHBoxLayout(header_);
  bar->setContentsMargins(4, 9, 12, 9);  // Controls centred at y=27, level with the window buttons.
  bar->setSpacing(8);
  layoutControl_ = new Segmented({{"layout-default", t("layout.grid")}, {"layout-masonry", t("layout.masonry")}, {"layout-apple", t("layout.apple")}}, t("layout.label"));
  sortButton_ = new TextButton(t("sort.name"), TextButton::Ghost);
  sortButton_->setRadius(Theme::kRadiusLg);
  sortButton_->setFixedHeight(32);
  sortButton_->setFont(Theme::font(14));
  sortButton_->setTrailingIcon("chevron-down");
  directionButton_ = new IconButton("sort-ascending", t("sort.ascending"), 32);
  directionButton_->setRadius(Theme::kRadiusLg);
  favoritesButton_ = new IconButton("heart", t("favorites.show"), 36);
  favoritesButton_->setRadius(Theme::kRadius3xl);
  columnsSlider_ = new NumberSlider(2, 12, ui_.columns, t("columns.label"));
  dupButton_ = new TextButton(t("duplicates.find"), TextButton::Ghost);
  dupButton_->setRadius(Theme::kRadiusLg);
  dupButton_->setFixedHeight(32);
  dupButton_->setFont(Theme::font(14));
  dupButton_->setIcon("stack-2");
  dupButton_->setCheckable(true);
  dupButton_->hide();
  albumMenuButton_ = new IconButton("dots", t("album.actions"), 36);
  bar->addWidget(layoutControl_);
  bar->addWidget(sortButton_);
  bar->addWidget(directionButton_);
  bar->addWidget(favoritesButton_);
  bar->addWidget(columnsSlider_);
  bar->addStretch(1);
  bar->addWidget(dupButton_);
  bar->addWidget(albumMenuButton_);
  rightLayout->addWidget(header_);
  stack_ = new QStackedWidget;
  gallery_ = new GalleryView(&model_, &pipeline_);
  duplicates_ = new DuplicatesView(&core_, &pipeline_);
  stack_->addWidget(gallery_);
  stack_->addWidget(duplicates_);
  rightLayout->addWidget(stack_, 1);
  gallery_->setColumns(ui_.columns);
  gallery_->setLayoutKind(ui_.layout == QLatin1String("masonry") ? GalleryView::Masonry : ui_.layout == QLatin1String("apple") ? GalleryView::Apple : GalleryView::Grid);
  layoutControl_->setCurrent(ui_.layout == QLatin1String("masonry") ? 1 : ui_.layout == QLatin1String("apple") ? 2 : 0);
  model_.sortKey = ui_.sortKey;
  model_.sortDir = ui_.sortDir;
  gallery_->setFrameCap(ui_.scroll60 ? 16 : qEnvironmentVariableIntValue("ROOM237_FRAME_CAP_MS"));
  sidebarHandle_ = new QWidget(host_);
  sidebarHandle_->setCursor(Qt::SplitHCursor);
  sidebarHandle_->installEventFilter(this);
  viewer_ = new Viewer(host_);
  preload_ = new PreloadOverlay(host_);
  // Worker counts from the processing screen: saved like any setting, and the core follows them at once.
  workersSave_.setSingleShot(true);
  workersSave_.setInterval(300);
  connect(preload_, &PreloadOverlay::workersChanged, this, [this] { workersSave_.start(); });
  connect(&workersSave_, &QTimer::timeout, this, [this] {
    core_.call(QStringLiteral("get_settings"), {}, [this](const QJsonValue &value, const QString &error) {
      if (!error.isEmpty()) return;
      QJsonObject settings = value.toObject();
      QJsonObject preload = settings.value(QLatin1String("preload")).toObject();
      preload[QStringLiteral("thumbWorkers")] = preloadWorkers_[0];
      preload[QStringLiteral("metaWorkers")] = preloadWorkers_[1];
      preload[QStringLiteral("encryptWorkers")] = preloadWorkers_[2];
      settings[QStringLiteral("preload")] = preload;
      core_.call(QStringLiteral("update_settings"), {{QStringLiteral("settings"), settings}}, [this](const QJsonValue &saved, const QString &failure) {
        if (failure.isEmpty() && saved.isObject()) onSettings(saved.toObject());
      });
    });
  });
  connect(preload_, &PreloadOverlay::workersChanged, this, [this](int thumbnails, int metadata, int encryption) { preloadWorkers_ = {thumbnails, metadata, encryption}; });
  connect(preload_, &PreloadOverlay::blockingChanged, this, &MainWindow::setProcessing);
  dragHint_ = new QLabel(host_);
  dragHint_->setFont(Theme::font(12));
  dragHint_->setStyleSheet(QStringLiteral("color:white;background:rgba(0,0,0,178);border:1px solid rgba(255,255,255,38);border-radius:12px;padding:4px 12px;"));
  dragHint_->setAttribute(Qt::WA_TransparentForMouseEvents);
  dragHint_->hide();
  directoryScreen_ = new HeroScreen(host_, QStringLiteral("choose_root"));
  directoryScreen_->setTexts(t("directory.title"), t("directory.subtitle"));
  retryRoot_ = new TextButton(t("common.retry"), TextButton::Outline);
  retryRoot_->hide();
  auto *chooseRoot = chooseRoot_ = new TextButton(t("directory.choose"), TextButton::Primary);
  directoryScreen_->addButton(retryRoot_);
  directoryScreen_->addButton(chooseRoot);
  directoryScreen_->hide();
  confirmScreen_ = new HeroScreen(host_, QStringLiteral("confirm_open"));
  auto *confirmOpen = confirmOpen_ = new TextButton(t("confirmOpen.open"), TextButton::Primary);
  auto *pickAnother = pickAnother_ = new TextButton(t("confirmOpen.pickAnother"), TextButton::Outline);
  confirmScreen_->addButton(confirmOpen);
  confirmScreen_->addButton(pickAnother);
  confirmScreen_->hide();
  for (HeroScreen *screen : {directoryScreen_, confirmScreen_}) {
    auto *gear = new IconButton("settings", t("settings.title"), 32, screen);
    gear->setObjectName(QStringLiteral("cornerSettings"));
    connect(gear, &QAbstractButton::clicked, this, &MainWindow::openSettings);
  }
  lockScreen_ = new HeroScreen(host_, QStringLiteral("lockscreen"), true);
  lockScreen_->setTexts(t("lock.title"), t("lock.subtitle"));
  lockScreen_->setSubtitleStyle(16, "rgba(255,255,255,128)");
  lockScreen_->hide();
  vaultScreen_ = new VaultUnlock(host_, &core_);
  connect(vaultScreen_, &VaultUnlock::unlocked, this, &MainWindow::onVaultUnlocked);
  vaultRefresh_.setSingleShot(true);
  vaultRefresh_.setInterval(1500);
  connect(&vaultRefresh_, &QTimer::timeout, this, [this] {
    if (!model_.root.isEmpty()) model_.reload({}, true);
  });
  lockFade_.setStartValue(1.0);
  lockFade_.setEndValue(0.0);
  connect(&lockFade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(lockScreen_->graphicsEffect())) effect->setOpacity(v.toReal());
  });
  connect(&lockFade_, &QVariantAnimation::finished, this, [this] {
    lockScreen_->setGraphicsEffect(nullptr);
    if (!locked_) lockScreen_->hide();
  });
  // The duplicates workspace fades in and rises by 8px.
  workspaceFade_.setStartValue(0.0);
  workspaceFade_.setEndValue(1.0);
  connect(&workspaceFade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
    duplicates_->setEntrance(v.toReal());
  });
  connect(&workspaceFade_, &QVariantAnimation::finished, this, [this] { duplicates_->setEntrance(1); });
  startupScreen_ = new StartupScreen(host_);
  shield_ = new PrivacyShield(host_);
  toasts_ = new ToastHost(host_);
  connect(startupScreen_, &StartupScreen::retry, this, [this] {
    startupScreen_->setBusy(t("startup.checking"));
    core_.call(QStringLiteral("retry_startup"));
  });
  const auto pick = [this] { pickRootDirectory(QStringLiteral("startup")); };
  connect(chooseRoot, &QAbstractButton::clicked, this, pick);
  connect(retryRoot_, &QAbstractButton::clicked, this, &MainWindow::showStartScreens);
  connect(pickAnother, &QAbstractButton::clicked, this, pick);
  connect(confirmOpen, &QAbstractButton::clicked, this, [this] {
    const QString root = privacyEnabled_ && !ui_.decoyRoot.isEmpty() ? ui_.decoyRoot : effectiveRoot();
    if (!rootAllowed(root)) return;
    if (privacyEnabled_ && !ui_.decoyRoot.isEmpty()) displayDecoy_ = true;
    allowOpen_ = true;
    confirmScreen_->hide();
    openRoot(effectiveRoot());
  });
  // A secondary click on Open always shows the real library.
  confirmOpen->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(confirmOpen, &QWidget::customContextMenuRequested, this, [this] {
    if (!rootAllowed(ui_.rootDir)) return;
    displayDecoy_ = false;
    allowOpen_ = true;
    confirmScreen_->hide();
    openRoot(effectiveRoot());
  });
  connect(&model_, &GalleryModel::albumsChanged, this, &MainWindow::rebuildTree);
  connect(&model_, &GalleryModel::albumsChanged, this, &MainWindow::updateStatus);
  connect(&model_, &GalleryModel::reset, this, &MainWindow::updateStatus);
  connect(gallery_, &GalleryView::openRequested, this, &MainWindow::openViewer);
  connect(gallery_, &GalleryView::contextRequested, this, &MainWindow::showMediaMenu);
  connect(gallery_, &GalleryView::dragFinished, this, [this] { tree_->collapseAutoExpandedExcept(model_.album); });
  connect(gallery_, &GalleryView::exportWithheld, this, [this] { confirmExport([this] { showToast(0, t("security.export.allowed")); }); });
  connect(gallery_, &GalleryView::selectionChanged, this, &MainWindow::updateStatus);
  connect(gallery_, &GalleryView::filesDropped, this, [this](const QStringList &paths) { importPaths(paths, model_.album); });
  connect(gallery_, &GalleryView::copyRequested, this, [this](int index) {
    if (const MediaItem *item = model_.item(index)) copyItems({*item});
  });
  connect(gallery_, &GalleryView::favoriteRequested, this, [this](int index) {
    if (const MediaItem *item = model_.item(index)) setFavorite(*item, !item->favorite);
  });
  connect(gallery_, &GalleryView::deleteRequested, this, [this](int index) {
    if (const MediaItem *item = model_.item(index)) deleteItems({*item});
  });
  connect(duplicates_, &DuplicatesView::mutationStarted, this, [this] { batchRunning_ = true; });
  connect(duplicates_, &DuplicatesView::mutationFinished, this, [this] {
    batchRunning_ = false;
    afterMutation({model_.album}, {});
  });
  connect(duplicates_, &DuplicatesView::closeRequested, this, [this] { showDuplicates(false); });
  connect(duplicates_, &DuplicatesView::failure, this, [this](const QString &message) { showToast(1, message); });
  connect(duplicates_, &DuplicatesView::previewRequested, this, [this](int group, int item, const QVector<MediaItem> &members) {
    previewItems_ = members;
    previewGroup_ = group;
    viewerIndex_ = item;
    viewer_->setPosition(item, members.size());
    viewer_->showItem(members[item]);
  });
  connect(duplicates_, &DuplicatesView::changed, this, [this] {
    const int total = duplicates_->total();
    dupButton_->setVisible(total > 0 || duplicatesVisible());
  });
  connect(viewer_, &Viewer::navigate, this, &MainWindow::stepViewer);
  connect(viewer_, &Viewer::closing, this, [this] {
    // The list moves to the picture the viewer closes on, and the picture flies back into that tile.
    if (!previewItems_.isEmpty() || duplicatesVisible() || viewerIndex_ < 0 || viewerIndex_ >= model_.total()) return;
    gallery_->focusIndex(viewerIndex_);
    const QRect tile = gallery_->tileRect(viewerIndex_);
    viewer_->setOrigin(QRect(gallery_->viewport()->mapTo(host_, tile.topLeft()), tile.size()), gallery_->tileRadius());
    if (Theme::motion(1)) viewer_->setBackdrop(grabBehindViewer());
  });
  connect(viewer_, &Viewer::closed, this, &MainWindow::onViewerClosed);
  connect(viewer_, &Viewer::becameReady, this, &MainWindow::resolveNeighbors);
  connect(viewer_, &Viewer::favoriteRequested, this, [this](const MediaItem &item) { setFavorite(item, !item.favorite); });
  connect(viewer_, &Viewer::copyRequested, this, [this](const MediaItem &item) { copyItems({item}); });
  connect(viewer_, &Viewer::revealRequested, this, [this](const MediaItem &item) { Platform::revealInFileManager(item.path()); });
  connect(viewer_, &Viewer::deleteRequested, this, [this](const MediaItem &item) {
    const int index = viewerIndex_;
    const QVector<MediaItem> group = previewItems_;
    if (!group.isEmpty()) {
      previewItems_.clear();
      viewer_->closeViewer();
      duplicates_->deleteOne(previewGroup_, item);
      return;
    }
    deleteItems({item}, [this, index](const QString &error) {
      if (!error.isEmpty() || !viewer_->isOpen()) return;
      if (model_.total() == 0) viewer_->closeViewer();
      else openViewer(qMin(index, model_.total() - 1));
    });
  });
  connect(tree_, &AlbumTree::activated, this, [this](const QString &id) { activateAlbum(id); });
  connect(tree_, &AlbumTree::contextRequested, this, &MainWindow::showAlbumMenu);
  connect(tree_, &AlbumTree::mediaDropped, this, [this](const QString &album) {
    gallery_->collectSelection([this, album](const QVector<MediaItem> &items) { moveItems(items, album); });
  });
  connect(tree_, &AlbumTree::filesDropped, this, [this](const QStringList &paths, const QString &album) { importPaths(paths, album); });
  connect(tree_, &AlbumTree::albumDropped, this, [this](const QString &album, const QString &parent) {
    const QString name = album.section(QLatin1Char('/'), -1);
    moveAlbum(album, parent, [this, name](const QString &error) {
      if (!error.isEmpty()) showToast(1, t("toast.albumMoveFailed") + QStringLiteral(": ") + error);
      else showToast(0, t("toast.albumMoved", {{"album", name}}));
    });
  });
  connect(tree_, &AlbumTree::dragHint, this, [this](const QString &text, const QPoint &global) {
    if (text.isEmpty()) return dragHint_->hide();
    dragHint_->setText(text);
    dragHint_->adjustSize();
    const QPoint local = host_->mapFromGlobal(global);
    dragHint_->move(qBound(4, local.x() + 14, host_->width() - dragHint_->width() - 4), qMax(4, local.y() + 14));
    dragHint_->show();
    dragHint_->raise();
  });
  connect(tree_, &AlbumTree::newAlbumRequested, this, [this](const QRect &anchor) {
    QRect inHost(tree_->viewport()->mapTo(host_, anchor.topLeft()), anchor.size());
    // Beside the sidebar card, not over its edge.
    inHost.setRight(sidebar_->mapTo(host_, QPoint(sidebar_->width(), 0)).x());
    auto holder = std::make_shared<QPointer<TextPrompt>>();
    auto *dialog = new TextPrompt(host_, t("album.new"), t("album.create"), {}, [this, holder](const QString &name) {
      if (*holder) (*holder)->closeNow();
      createAlbum(effectiveRoot(), name);
    });
    *holder = dialog;
    dialog->openAt(inHost, Qt::RightEdge, Qt::AlignTop);
  });
  connect(keyboardButton_, &QAbstractButton::clicked, this, [this] {
    auto *panel = new HotkeyPanel(host_, !model_.album.isEmpty(), gallery_->selectionCount() > 0);
    panel->setTrigger(keyboardButton_);
    keyboardButton_->setHeld(true);
    connect(panel, &Popover::closed, keyboardButton_, [this] { keyboardButton_->setHeld(false); });
    panel->openAt(hostRect(host_, keyboardButton_), Qt::TopEdge, Qt::AlignLeft);
  });
  connect(settingsButton_, &QAbstractButton::clicked, this, &MainWindow::openSettings);
  connect(layoutControl_, &Segmented::activated, this, [this](int index) { setLayoutKind(index == 1 ? QStringLiteral("masonry") : index == 2 ? QStringLiteral("apple") : QStringLiteral("default")); });
  connect(sortButton_, &QAbstractButton::clicked, this, [this] {
    QVector<MenuItem> items;
    const std::pair<QString, const char *> options[] = {{QStringLiteral("shoot"), "sort.shoot"}, {QStringLiteral("added"), "sort.added"}, {QStringLiteral("name"), "sort.name"}, {QStringLiteral("random"), "sort.random"}};
    const QHash<QString, QByteArray> icons{{QStringLiteral("shoot"), "camera"}, {QStringLiteral("added"), "calendar"}, {QStringLiteral("name"), "file-text"}, {QStringLiteral("random"), "arrows-shuffle"}};
    for (const auto &[key, text] : options) items.push_back({icons.value(key), t(text), [this, key = key] { setSort(key); }, true, false, false, model_.sortKey == key});
    Menu::select(host_, items, hostRect(host_, sortButton_), Qt::AlignLeft, 190);
  });
  connect(directionButton_, &QAbstractButton::clicked, this, [this] { setSortDirection(model_.sortDir == QLatin1String("asc") ? QStringLiteral("desc") : QStringLiteral("asc")); });
  connect(favoritesButton_, &QAbstractButton::clicked, this, [this] {
    model_.favoritesOnly = !model_.favoritesOnly;
    updateHeader();
    model_.reload();
  });
  connect(columnsSlider_, &NumberSlider::valueChanged, this, [this](int value) {
    gallery_->setColumns(value);
    ui_.columns = value;
    saveTimer_.start();
  });
  // toggled, not clicked: an accessibility toggle changes the state without a click.
  connect(dupButton_, &QAbstractButton::toggled, this, [this](bool on) { if (on != duplicatesVisible()) showDuplicates(on); });
  connect(albumMenuButton_, &QAbstractButton::clicked, this, [this] {
    const AlbumInfo *found = model_.favoritesAlbum ? nullptr : model_.albumById(model_.album);
    if (!found) return;
    const AlbumInfo album = *found;
    auto *panel = new AlbumActionsPanel(host_, album.name, Platform::fileManagerIcon(), [album] { Platform::revealInFileManager(album.path); }, [this, album] {
      QGuiApplication::clipboard()->setText(album.path);
      showToast(0, t("toast.pathCopied"));
    }, [this, album] {
      deleteAlbum(album.id, [this](const QString &error) {
        if (!error.isEmpty()) showToast(1, error);
      });
    });
    panel->setTrigger(albumMenuButton_);
    albumMenuButton_->setHeld(true);
    connect(panel, &Popover::closed, albumMenuButton_, [this] { albumMenuButton_->setHeld(false); });
    panel->openAt(hostRect(host_, albumMenuButton_), Qt::BottomEdge, Qt::AlignRight);
  });
}
void MainWindow::buildActions() {
  // The text key stays on the action, so a language change can retitle the menus.
  const auto add = [this](const QKeySequence &key, std::function<void()> run, const char *textKey = nullptr) {
    auto *action = new QAction(textKey ? t(textKey) : QString(), this);
    if (textKey) action->setProperty("i18nKey", QByteArray(textKey));
    action->setShortcut(key);
    action->setShortcutContext(Qt::WindowShortcut);
    connect(action, &QAction::triggered, this, std::move(run));
    addAction(action);
    return action;
  };
  logger_ = new LoggerPanel(host_);
  QAction *selectAll = add(QKeySequence::SelectAll, [this] { if (galleryKeysAllowed()) gallery_->selectAll(); }, "hotkeys.selectAll");
  QAction *clearSelection = add(QKeySequence(Qt::CTRL | Qt::Key_D), [this] { if (galleryKeysAllowed()) gallery_->clearSelection(); }, "hotkeys.clearSelection");
  QAction *copy = add(QKeySequence::Copy, [this] {
    if (viewer_->isOpen()) return copyItems({viewer_->item()});
    if (galleryKeysAllowed() && gallery_->selectionCount() > 0) gallery_->collectSelection([this](const QVector<MediaItem> &items) { copyItems(items); });
  }, "hotkeys.copyToClipboard");
  QAction *paste = add(QKeySequence::Paste, [this] { if (galleryKeysAllowed()) pasteFromClipboard(); }, "menu.paste");
  const auto removeSelected = [this] {
    if (galleryKeysAllowed() && gallery_->selectionCount() > 0) gallery_->collectSelection([this](const QVector<MediaItem> &items) { deleteItems(items); });
  };
#ifdef Q_OS_MACOS
  QAction *remove = add(QKeySequence(Qt::CTRL | Qt::Key_Backspace), removeSelected, "hotkeys.deleteSelection");
  add(QKeySequence(Qt::Key_Delete), removeSelected);
#else
  QAction *remove = add(QKeySequence(Qt::Key_Delete), removeSelected, "hotkeys.deleteSelection");
#endif
  QAction *refreshAction = add(QKeySequence::Refresh, [this] { refresh(); }, "menu.refresh");
  QAction *settings = add(QKeySequence(Qt::CTRL | Qt::Key_Comma), [this] { openSettings(); }, "settings.title");
  settings->setMenuRole(QAction::PreferencesRole);
  QAction *fullscreen = add(QKeySequence::FullScreen, [this] { isFullScreen() ? showNormal() : showFullScreen(); }, "viewer.fullscreen");
  QAction *grid = add(QKeySequence(Qt::CTRL | Qt::Key_1), [this] { setLayoutKind(QStringLiteral("default")); }, "layout.grid");
  QAction *masonry = add(QKeySequence(Qt::CTRL | Qt::Key_2), [this] { setLayoutKind(QStringLiteral("masonry")); }, "layout.masonry");
  QAction *apple = add(QKeySequence(Qt::CTRL | Qt::Key_3), [this] { setLayoutKind(QStringLiteral("apple")); }, "layout.apple");
  QAction *importFiles = add(QKeySequence(Qt::CTRL | Qt::Key_O), [this] {
    if (!galleryKeysAllowed() || model_.favoritesAlbum || model_.album.isEmpty()) return;
    const QStringList files = QFileDialog::getOpenFileNames(this, t("menu.importFiles"));
    if (!files.isEmpty()) importPaths(files, model_.album);
  }, "menu.importFiles");
#ifdef Q_OS_MACOS
  // Native menu bar with the shortcuts above.
  auto *bar = menuBar();
  QMenu *file = bar->addMenu(t("menu.file"));
  file->menuAction()->setProperty("i18nKey", QByteArray("menu.file"));
  file->addAction(importFiles);
  file->addAction(settings);
  QMenu *edit = bar->addMenu(t("menu.edit"));
  edit->menuAction()->setProperty("i18nKey", QByteArray("menu.edit"));
  edit->addAction(copy);
  edit->addAction(paste);
  edit->addAction(remove);
  edit->addSeparator();
  edit->addAction(selectAll);
  edit->addAction(clearSelection);
  QMenu *view = bar->addMenu(t("menu.view"));
  view->menuAction()->setProperty("i18nKey", QByteArray("menu.view"));
  view->addAction(grid);
  view->addAction(masonry);
  view->addAction(apple);
  view->addSeparator();
  view->addAction(refreshAction);
  view->addAction(fullscreen);
#else
  Q_UNUSED(selectAll);
  Q_UNUSED(clearSelection);
  Q_UNUSED(copy);
  Q_UNUSED(paste);
  Q_UNUSED(remove);
  Q_UNUSED(refreshAction);
  Q_UNUSED(fullscreen);
  Q_UNUSED(grid);
  Q_UNUSED(masonry);
  Q_UNUSED(apple);
  Q_UNUSED(importFiles);
  menuBar()->hide();
#endif
}
void MainWindow::loadUiState() {
  QFile file(QDir(profile_).filePath(QStringLiteral("ui-state.json")));
  if (!file.open(QIODevice::ReadOnly)) return;
  const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
  if (!QaFence::rootsAllowed(o)) {
    uiRootsRefused_ = true;
    return;
  }
  uiRootsRefused_ = false;
  ui_.columns = qBound(2, o.value(QLatin1String("columns")).toInt(ui_.columns), 12);
  const QString sortKey = o.value(QLatin1String("sortKey")).toString();
  if (QStringList{QStringLiteral("shoot"), QStringLiteral("added"), QStringLiteral("name"), QStringLiteral("random")}.contains(sortKey)) ui_.sortKey = sortKey;
  const QString sortDir = o.value(QLatin1String("sortDir")).toString();
  if (sortDir == QLatin1String("asc") || sortDir == QLatin1String("desc")) ui_.sortDir = sortDir;
  ui_.rootDir = o.value(QLatin1String("rootDir")).toString();
  ui_.decoyRoot = o.value(QLatin1String("decoyRoot")).toString();
  ui_.contentProtected = o.value(QLatin1String("contentProtected")).toBool();
  ui_.hardeningNeverAsk.clear();
  for (const QJsonValue &root : o.value(QLatin1String("hardeningNeverAsk")).toArray()) ui_.hardeningNeverAsk << root.toString();
  if (const QString language = o.value(QLatin1String("language")).toString(); language == QLatin1String("en") || language == QLatin1String("ru")) ui_.language = language;
  if (const QString layout = o.value(QLatin1String("layout")).toString(); layout == QLatin1String("default") || layout == QLatin1String("apple") || layout == QLatin1String("masonry")) ui_.layout = layout;
  ui_.scroll60 = o.value(QLatin1String("scroll60hz")).toBool();
  ui_.autoUpdate = o.value(QLatin1String("autoUpdate")).toBool(true);
  ui_.sidebarFraction = qBound(0.16, o.value(QLatin1String("sidebarFraction")).toDouble(ui_.sidebarFraction), 0.24);
}
void MainWindow::saveUiState() {
  if (uiRootsRefused_ || !QaFence::rootsAllowed({{QStringLiteral("rootDir"), ui_.rootDir}, {QStringLiteral("decoyRoot"), ui_.decoyRoot}})) return;
  const QJsonObject o{{QStringLiteral("columns"), ui_.columns}, {QStringLiteral("sortKey"), ui_.sortKey}, {QStringLiteral("sortDir"), ui_.sortDir}, {QStringLiteral("rootDir"), ui_.rootDir.isEmpty() ? QJsonValue() : QJsonValue(ui_.rootDir)}, {QStringLiteral("decoyRoot"), ui_.decoyRoot.isEmpty() ? QJsonValue() : QJsonValue(ui_.decoyRoot)}, {QStringLiteral("contentProtected"), ui_.contentProtected}, {QStringLiteral("language"), ui_.language}, {QStringLiteral("layout"), ui_.layout}, {QStringLiteral("scroll60hz"), ui_.scroll60}, {QStringLiteral("autoUpdate"), ui_.autoUpdate}, {QStringLiteral("sidebarFraction"), ui_.sidebarFraction}, {QStringLiteral("hardeningNeverAsk"), QJsonArray::fromStringList(ui_.hardeningNeverAsk)}};
  QSaveFile file(QDir(profile_).filePath(QStringLiteral("ui-state.json")));
  if (file.open(QIODevice::WriteOnly)) {
    file.write(QJsonDocument(o).toJson());
    file.commit();
  }
}
void MainWindow::showToast(int kind, const QString &text) { toasts_->show(kind == 0 ? ToastHost::Success : kind == 1 ? ToastHost::Error : ToastHost::Info, text); }
void MainWindow::onEvent(const QString &name, const QJsonValue &payload) {
  const QJsonObject o = payload.toObject();
  if (name == QLatin1String("startup-status")) {
    const QString phase = o.value(QLatin1String("phase")).toString();
    if (phase == QLatin1String("ready") && !coreReady_) {
      coreReady_ = true;
      // The core has imported the old profile by now.
      loadUiState();
      I18n::setLanguage(ui_.language);
      refreshTexts();
      core_.call(QStringLiteral("get_settings"), {}, [this](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) {
          startupScreen_->setError(t("settings.loadFailed"), error);
          return;
        }
        onSettings(value.toObject());
        settingsLoaded_ = true;
        startupScreen_->hide();
        emit coreBecameReady();
        showStartScreens();
      });
    } else if (phase == QLatin1String("error")) {
      startupScreen_->setError(t("startup.error"), o.value(QLatin1String("message")).toString());
    }
  } else if (name == QLatin1String("vault-progress")) {
    onVaultProgress(o);
  } else if (name == QLatin1String("hash-progress")) {
    hashDone_ = o.value(QLatin1String("completed")).toInt();
    hashTotal_ = o.value(QLatin1String("total")).toInt();
    updateHashing();
  } else if (name == QLatin1String("media-processed")) {
    processed_++;
    if (!thumbRefresh_.isActive()) thumbRefresh_.start();
  } else if (name == QLatin1String("preload-progress")) {
    const QJsonObject thumbs = o.value(QLatin1String("thumbnails")).toObject(), meta = o.value(QLatin1String("metadata")).toObject();
    const int thumbsDone = thumbs.value(QLatin1String("completed")).toInt(), thumbsTotal = thumbs.value(QLatin1String("total")).toInt(), metaDone = meta.value(QLatin1String("completed")).toInt(), metaTotal = meta.value(QLatin1String("total")).toInt();
    const bool finished = thumbsDone >= thumbsTotal && metaDone >= metaTotal;
    if (finished || o.value(QLatin1String("stage")).toString() == QLatin1String("idle")) {
      preload_->finish();
      // Album covers exist only after their previews; the tree listed before indexing shows none.
      if (preloadRunning_ && albumsReady_) model_.refreshAlbums();
      preloadRunning_ = false;
    } else {
      preloadRunning_ = true;
      // A handful of new files is processed behind the gallery; the screen is for a library being indexed.
      if (o.value(QLatin1String("active_actions")).toInt() >= 100) preload_->setStages(thumbsDone, thumbsTotal, metaDone, metaTotal);
      else preload_->finish();
    }
  }
}
void MainWindow::updateHashing() {
  const bool active = hashTotal_ - hashDone_ > 3;
  hashPill_->setVisible(active);
  if (active) hashPill_->setProgress(QStringLiteral("%1 %2%").arg(t("albums.hashing")).arg(hashDone_ * 100 / qMax(1, hashTotal_)), double(hashDone_) / qMax(1, hashTotal_));
  else if (hashTotal_ > 0 && hashDone_ >= hashTotal_) probeDuplicates();
}
void MainWindow::onSettings(const QJsonObject &settings) {
  const QJsonObject preload = settings.value(QLatin1String("preload")).toObject();
  preloadWorkers_ = {preload.value(QLatin1String("thumbWorkers")).toInt(4), preload.value(QLatin1String("metaWorkers")).toInt(4), preload.value(QLatin1String("encryptWorkers")).toInt(4)};
  preload_->setWorkers(preloadWorkers_[0], preloadWorkers_[1], preloadWorkers_[2]);
  const QJsonObject privacy = settings.value(QLatin1String("privacy")).toObject();
  privacyEnabled_ = privacy.value(QLatin1String("enabled")).toBool();
  lockscreenEnabled_ = privacyEnabled_ || privacy.value(QLatin1String("lockscreenEnabled")).toBool();
  confirmOpenEnabled_ = privacyEnabled_ || privacy.value(QLatin1String("confirmOpenEnabled")).toBool();
  const bool hardeningBefore = hardening_;
  hardening_ = privacy.value(QLatin1String("hardening")).toBool();
  lockscreenEnabled_ = lockscreenEnabled_ || hardening_;
  updatePrivacySurface();
  if (hardening_ && !hardeningBefore && settingsLoaded_) {
    offeredRoots_.clear();
    offerEncryption();
  }
  if (lockscreenEnabled_) inactivity_.start();
  else {
    inactivity_.stop();
    if (locked_) setLocked(false);
  }
}
void MainWindow::updatePrivacySurface() {
  // An encrypted root always blocks screenshots, also behind its unlock screen.
  const bool vaultShown = inProtectedRoot() || vaultScreen_->isVisible();
  if (QGuiApplication::platformName() != QLatin1String("offscreen")) Platform::setContentProtected(this, (privacyEnabled_ && ui_.contentProtected) || vaultShown);
  applyExportGate();
  if (!privacyEnabled_ && unfocused_) setUnfocused(false);
  if (privacyEnabled_ && !isActiveWindow() && QGuiApplication::applicationState() != Qt::ApplicationActive) setUnfocused(true);
}
void MainWindow::showStartScreens() {
  // QA only: a profile with its root inside the fence shows the start screens instead of opening --root at once.
  const bool holdStart = QaFence::enabled() && qEnvironmentVariableIsSet("ROOM237_QA_HOLD_START");
  const QString root = !cliRoot_.isEmpty() && !holdStart ? cliRoot_ : ui_.rootDir;
  if (!root.isEmpty() && !QFileInfo(root).isDir()) {
    showMissingRoot(root);
    return;
  }
  missingRoot_.clear();
  updateDirectoryTexts();
  if (!cliRoot_.isEmpty() && !holdStart) {
    allowOpen_ = true;
    openRoot(cliRoot_);
    return;
  }
  if (ui_.rootDir.isEmpty()) {
    directoryScreen_->setGeometry(host_->rect());
    directoryScreen_->show();
    directoryScreen_->raise();
    return;
  }
  // An encrypted root asks for its password instead.
  if (confirmOpenEnabled_ && !vaultStatus(ui_.rootDir).value(QLatin1String("vault")).toBool()) {
    confirmScreen_->setTexts(t("confirmOpen.title"), t("confirmOpen.subtitle", {{"folder", QFileInfo(ui_.rootDir).fileName().toHtmlEscaped()}}));
    confirmScreen_->setGeometry(host_->rect());
    confirmScreen_->show();
    confirmScreen_->raise();
    return;
  }
  allowOpen_ = true;
  openRoot(ui_.rootDir);
}
void MainWindow::showMissingRoot(const QString &root) {
  // The library may sit on an unmounted drive or a locked encrypted volume: keep it saved and offer a retry.
  missingRoot_ = root;
  allowOpen_ = false;
  viewer_->closeViewer();
  if (duplicatesVisible()) showDuplicates(false);
  watcher_.stop();
  model_.setRoot(QString());
  model_.album.clear();
  model_.favoritesAlbum = false;
  model_.reload();
  albumsReady_ = false;
  confirmScreen_->hide();
  updateDirectoryTexts();
  directoryScreen_->setGeometry(host_->rect());
  directoryScreen_->show();
  directoryScreen_->raise();
  // The lock screen and the privacy shield stay on top: the path of the library is private too.
  if (locked_) lockScreen_->raise();
  if (shield_->isVisible()) shield_->raise();
}
void MainWindow::updateDirectoryTexts() {
  retryRoot_->setVisible(!missingRoot_.isEmpty());
  if (missingRoot_.isEmpty()) directoryScreen_->setTexts(t("directory.title"), t("directory.subtitle"));
  else directoryScreen_->setTexts(t("directory.missingTitle"), t("directory.missingSubtitle", {{"folder", QDir::toNativeSeparators(missingRoot_).toHtmlEscaped()}}));
}
void MainWindow::refreshTexts() {
  sortButton_->setText(model_.sortKey == QLatin1String("shoot") ? t("sort.shoot") : model_.sortKey == QLatin1String("added") ? t("sort.added") : model_.sortKey == QLatin1String("random") ? t("sort.random") : t("sort.name"));
  sortButton_->updateGeometry();
  updateDirectoryTexts();
  confirmScreen_->setTexts(t("confirmOpen.title"), t("confirmOpen.subtitle", {{"folder", QFileInfo(ui_.rootDir).fileName().toHtmlEscaped()}}));
  retryRoot_->setText(t("common.retry"));
  chooseRoot_->setText(t("directory.choose"));
  confirmOpen_->setText(t("confirmOpen.open"));
  pickAnother_->setText(t("confirmOpen.pickAnother"));
  keyboardButton_->setLabel(t("hotkeys.show"));
  settingsButton_->setLabel(t("settings.title"));
  albumMenuButton_->setLabel(t("album.actions"));
  layoutControl_->setLabels(t("layout.label"), {t("layout.grid"), t("layout.masonry"), t("layout.apple")});
  columnsSlider_->setAccessibleName(t("columns.label"));
  columnsSlider_->setToolTip(t("columns.label"));
  gallery_->setAccessibleName(t("media.selectAlbum"));
  viewer_->retranslate();
  duplicates_->retranslate();
  for (QAction *action : findChildren<QAction *>())
    if (const QByteArray key = action->property("i18nKey").toByteArray(); !key.isEmpty()) action->setText(t(key.constData()));
  lockScreen_->setTexts(t("lock.title"), t("lock.subtitle"));
  vaultScreen_->retranslate();
  preload_->retranslate();
  tree_->setAlbums(model_.albums, model_.favoriteCount);
  dupButton_->setText(t("duplicates.find"));
  layoutControl_->update();
  updateHeader();
  updateStatus();
}
void MainWindow::applyLanguage(const QString &language) {
  ui_.language = language;
  I18n::setLanguage(language);
  refreshTexts();
  saveTimer_.start();
}
void MainWindow::setLayoutKind(const QString &layout) {
  if (ui_.layout == layout) return;
  ui_.layout = layout;
  gallery_->setLayoutKind(layout == QLatin1String("masonry") ? GalleryView::Masonry : layout == QLatin1String("apple") ? GalleryView::Apple : GalleryView::Grid);
  layoutControl_->setCurrent(layout == QLatin1String("masonry") ? 1 : layout == QLatin1String("apple") ? 2 : 0);
  saveTimer_.start();
}
void MainWindow::setSort(const QString &key) {
  if (key == QLatin1String("random")) model_.randomSeed = QRandomGenerator::global()->generateDouble();
  model_.sortKey = key;
  ui_.sortKey = key;
  saveTimer_.start();
  updateHeader();
  model_.reload();
}
void MainWindow::setSortDirection(const QString &direction) {
  model_.sortDir = direction;
  ui_.sortDir = direction;
  saveTimer_.start();
  updateHeader();
  model_.reload();
}
void MainWindow::updateHeader() {
  const bool enabled = (!model_.album.isEmpty() || model_.favoritesAlbum) && !duplicatesVisible();
  for (QWidget *control : {static_cast<QWidget *>(layoutControl_), static_cast<QWidget *>(sortButton_), static_cast<QWidget *>(directionButton_), static_cast<QWidget *>(favoritesButton_), static_cast<QWidget *>(columnsSlider_)}) control->setEnabled(enabled);
  sortButton_->setText(model_.sortKey == QLatin1String("shoot") ? t("sort.shoot") : model_.sortKey == QLatin1String("added") ? t("sort.added") : model_.sortKey == QLatin1String("random") ? t("sort.random") : t("sort.name"));
  sortButton_->setIcon(model_.sortKey == QLatin1String("shoot") ? "camera" : model_.sortKey == QLatin1String("added") ? "calendar" : model_.sortKey == QLatin1String("random") ? "arrows-shuffle" : "file-text");
  sortButton_->setFixedWidth(sortButton_->sizeHint().width());
  dupButton_->setFixedWidth(dupButton_->sizeHint().width() - 4);
  const bool ascending = model_.sortDir == QLatin1String("asc");
  directionButton_->setIcon(ascending ? "sort-ascending" : "sort-descending");
  directionButton_->setLabel(ascending ? t("sort.ascending") : t("sort.descending"));
  directionButton_->setVisible(model_.sortKey != QLatin1String("random"));
  const bool album = !model_.album.isEmpty() && !model_.favoritesAlbum;
  favoritesButton_->setVisible(album && (model_.favoritesOnly || favoriteCounts_.value(model_.album) > 0));
  favoritesButton_->setIcon(model_.favoritesOnly ? "heart-filled" : "heart");
  favoritesButton_->setTint(model_.favoritesOnly ? Theme::red400 : QColor());
  favoritesButton_->setActiveStyle(model_.favoritesOnly ? QColor(251, 44, 54, 26) : QColor(), model_.favoritesOnly ? QColor(251, 44, 54, 153) : QColor());
  favoritesButton_->setLabel(model_.favoritesOnly ? t("favorites.showAll") : t("favorites.show"));
  albumMenuButton_->setVisible(album);
  {
    const QSignalBlocker quiet(dupButton_);
    dupButton_->setChecked(duplicatesVisible());
  }
}
void MainWindow::updateStatus() {
  if (model_.total() == 0 && (!model_.album.isEmpty() || model_.favoritesAlbum)) {
    if (model_.favoritesOnly || model_.favoritesAlbum) gallery_->setEmptyHint(t("favorites.empty"), {}, "heart");
    else gallery_->setEmptyHint(t("media.empty.title"), t("media.empty.subtitle"), "upload");
  } else if (model_.album.isEmpty() && !model_.favoritesAlbum) {
    // Until the first listing of a library has answered, an empty list says nothing about the library.
    if (albumsReady_ && model_.albums.isEmpty() && model_.favoriteCount == 0) gallery_->setEmptyHint(t("media.noAlbums"), t("media.noAlbumsHint"), "folder-plus");
    else gallery_->setEmptyHint(t("media.selectAlbum"), t("media.selectAlbumHint"), "layout-grid");
  }
}
bool MainWindow::rootAllowed(const QString &root) const {
  return !QaFence::enabled() || (QaFence::allows(root) && QFileInfo(root).isDir());
}
QString MainWindow::rootDialogStart(bool decoy) const {
  QStringList candidates;
  if (decoy) candidates << ui_.decoyRoot;
  candidates << model_.root << effectiveRoot() << cliRoot_;
  for (const QString &path : candidates)
    if (!path.isEmpty() && QFileInfo(path).isDir() && rootAllowed(path)) return QFileInfo(path).canonicalFilePath();
  return QaFence::enabled() ? QString() : QDir::homePath();
}
void MainWindow::pickRootDirectory(const QString &kind, [[maybe_unused]] const QString &qaStart) {
  const QString title = kind == QLatin1String("decoy") ? t("settings.decoyGallery") : t("directory.title");
  QString chosen;
#ifdef ROOM237_QA
  if (QaFence::enabled()) {
    const bool qaStartAllowed = !qaStart.isEmpty() && QaFence::allows(qaStart) && QFileInfo(qaStart).isDir();
    const QString start = qaStartAllowed ? QFileInfo(qaStart).canonicalFilePath() : rootDialogStart(kind == QLatin1String("decoy"));
    if (start.isEmpty()) {
      qWarning("QA fence refused directory dialog without a valid start");
      return;
    }
    // Keep an exact owned dialog for scoped cancellation even if AppKit cannot expose a live URL.
    if (qaRootDialog_) return;
    QFileDialog dialog(this, title, start);
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly);
    qaRootDialog_ = &dialog;
    if (dialog.exec() == QDialog::Accepted && dialog.selectedFiles().size() == 1) chosen = dialog.selectedFiles().first();
    qaRootDialog_ = nullptr;
    if (!chosen.isEmpty()) acceptChosenRoot(kind, chosen);
    return;
  }
#endif
  chosen = QFileDialog::getExistingDirectory(this, title, rootDialogStart(kind == QLatin1String("decoy")));
  if (!chosen.isEmpty()) acceptChosenRoot(kind, chosen);
}
void MainWindow::acceptChosenRoot(const QString &kind, const QString &path, std::function<void(QString)> done) {
  if (kind != QLatin1String("root") && kind != QLatin1String("startup") && kind != QLatin1String("decoy")) {
    if (done) done(QStringLiteral("unknown root kind"));
    return;
  }
  if (!rootAllowed(path)) {
    if (done) done(QStringLiteral("QA fence refused chosen root"));
    return;
  }
  const auto accept = [this, kind, done](const QString &root) {
    // QA: check the canonical path returned by the core again before saving it.
    if (!rootAllowed(root)) {
      if (done) done(QStringLiteral("QA fence refused canonical root"));
      return;
    }
    const QString chosen = QaFence::enabled() ? QFileInfo(root).canonicalFilePath() : root;
    if (!rootAllowed(chosen)) {
      if (done) done(QStringLiteral("QA fence refused resolved root"));
      return;
    }
    if (kind == QLatin1String("decoy")) ui_.decoyRoot = chosen;
    else {
      const bool wasDecoy = displayDecoy_;
      displayDecoy_ = false;
      if (!openRoot(chosen)) {
        displayDecoy_ = wasDecoy;
        if (done) done(QStringLiteral("root is not available"));
        return;
      }
      ui_.rootDir = chosen;
      if (kind == QLatin1String("startup")) {
        allowOpen_ = true;
        confirmScreen_->hide();
        directoryScreen_->hide();
      }
    }
    saveTimer_.start();
    if (done) done({});
  };
  if (kind == QLatin1String("startup")) {
    core_.call(QStringLiteral("canonicalize_directory"), {{QStringLiteral("directory"), path}}, [accept, path, done](const QJsonValue &value, const QString &error) {
      if (QaFence::enabled() && (!error.isEmpty() || !value.isString())) {
        if (done) done(QStringLiteral("QA fence refused failed canonicalization"));
        return;
      }
      accept(value.isString() ? value.toString() : path);
    });
  } else accept(path);
}
bool MainWindow::switchDecoy() {
  if (!rootAllowed(ui_.decoyRoot)) return false;
  const bool previous = displayDecoy_;
  displayDecoy_ = true;
  if (openRoot(effectiveRoot())) return true;
  displayDecoy_ = previous;
  return false;
}
bool MainWindow::clearDecoy() {
  const bool wasShowing = displayDecoy_;
  const QString root = ui_.rootDir.isEmpty() ? cliRoot_ : ui_.rootDir;
  if (wasShowing && !rootAllowed(root)) return false;
  ui_.decoyRoot.clear();
  displayDecoy_ = false;
  saveTimer_.start();
  if (wasShowing) openRoot(root);
  return true;
}
bool MainWindow::openRoot(const QString &root) {
  if (!rootAllowed(root)) return false;
  const QString active = QaFence::enabled() ? QFileInfo(root).canonicalFilePath() : root;
  if (!rootAllowed(active)) return false;
  if (!QFileInfo(active).isDir()) {
    // Switching from an open library keeps it; with nothing open the start screen explains what is missing.
    if (model_.root.isEmpty()) showMissingRoot(active);
    else showToast(1, t("toast.folderMissing", {{"folder", QDir::toNativeSeparators(QDir::cleanPath(active))}}));
    return false;
  }
  missingRoot_.clear();
  directoryScreen_->hide();
  const QJsonObject vault = vaultStatus(active);
  if (vault.value(QLatin1String("vault")).toBool() && vault.value(QLatin1String("locked")).toBool()) {
    // Opening continues once the root is unlocked.
    showVaultUnlock(active);
    return true;
  }
  // Leaving an encrypted root locks it: its keys and decrypted copies do not outlive the visit.
  if (!vaultRoot_.isEmpty() && vaultRoot_ != active) {
    core_.call(QStringLiteral("vault_lock"), {{QStringLiteral("root"), vaultRoot_}});
    pipeline_.clear();
    tree_->dropIcons();
    SecureMedia::clearExports();
  }
  vaultRoot_ = vault.value(QLatin1String("vault")).toBool() ? active : QString();
  vaultScreen_->hide();
  if (inProtectedRoot()) inactivity_.start();
  exportAllowed_ = false;
  updatePrivacySurface();
  model_.setRoot(active);
  model_.album.clear();
  model_.favoritesAlbum = false;
  // Nothing from the previous library stays on screen while the new one lists its albums.
  model_.albums.clear();
  model_.favoriteCount = 0;
  favoriteCounts_.clear();
  model_.reload();
  albumsReady_ = false;
  viewer_->closeViewer();
  if (duplicatesVisible()) showDuplicates(false);
  dupButton_->hide();
  core_.call(QStringLiteral("set_allow_open"), {{QStringLiteral("allow"), true}});
  watcher_.start(model_.root);
  rebuildTree();
  updateStatus();
  updateHeader();
  model_.refreshAlbums([this] {
    albumsReady_ = true;
    updateStatus();
    updateWatcher();
    refreshFavoriteCounts();
    if (!healthyMarked_) {
      healthyMarked_ = true;
      updater_->markHealthyIfUpdating();
      updater_->start();
      emit healthy();
    }
    offerEncryption();
    // Open the first album when none is selected.
    if (model_.album.isEmpty() && !model_.albums.isEmpty() && !QaFence::enabled()) {
      for (const AlbumInfo &album : model_.albums)
        if (album.size > 0) return activateAlbum(album.id);
      activateAlbum(model_.albums.first().id);
    }
  });
  return true;
}
void MainWindow::refreshFavoriteCounts() {
  model_.refreshFavoriteCount();
  core_.call(QStringLiteral("favorite_counts"), {{QStringLiteral("rootDir"), model_.root}}, [this](const QJsonValue &value, const QString &) {
    favoriteCounts_.clear();
    const QJsonObject counts = value.toObject();
    for (auto it = counts.begin(); it != counts.end(); ++it) favoriteCounts_.insert(it.key(), it.value().toInt());
    updateHeader();
  });
}
void MainWindow::followActiveAlbum() {
  QStringList ids;
  ids.reserve(model_.albums.size());
  for (const AlbumInfo &album : model_.albums) ids << album.id;
  const bool albumsMoved = ids != knownAlbumIds_;
  knownAlbumIds_ = ids;
  if (model_.favoritesAlbum) {
    // Favorites span albums, so their paths move with any of them.
    if (albumsMoved && !knownAlbumIds_.isEmpty()) {
      if (viewer_->isOpen() && !QFileInfo::exists(viewer_->currentPath())) viewer_->closeViewer();
      model_.reload({}, true, true);
      refreshFavoriteCounts();
    }
    return;
  }
  if (model_.album.isEmpty()) {
    activeDirectoryId_.clear();
    return;
  }
  const AlbumInfo *same = model_.albumById(model_.album);
  if (same && (activeDirectoryId_.isEmpty() || same->directoryId == activeDirectoryId_)) {
    activeDirectoryId_ = same->directoryId;
    activeMissingAfter_ = -1;
    return;
  }
  const AlbumInfo *moved = nullptr;
  if (!activeDirectoryId_.isEmpty())
    for (const AlbumInfo &album : model_.albums)
      if (album.directoryId == activeDirectoryId_) moved = &album;
  // A listing that ran during a move can miss the album at both places. Only a listing started after
  // the miss was seen may close the album; answers to older listings are ignored.
  if (!moved && !same && !activeDirectoryId_.isEmpty()) {
    if (activeMissingAfter_ < 0) {
      activeMissingAfter_ = model_.listingsStarted();
      QTimer::singleShot(400, this, [this] { model_.refreshAlbums(); });
      return;
    }
    if (model_.listingAnswered() <= activeMissingAfter_) return;
  }
  activeMissingAfter_ = -1;
  // Open paths point at the old location either way.
  viewer_->closeViewer();
  if (!moved) {
    if (same) {
      activeDirectoryId_ = same->directoryId;  // Another folder took the name.
      if (duplicatesVisible()) showDuplicates(false);
      gallery_->clearSelection();
      model_.reload([this] { updateStatus(); probeDuplicates(); });
      return;
    }
    if (duplicatesVisible()) showDuplicates(false);
    gallery_->clearSelection();
    model_.album.clear();
    model_.favoritesOnly = false;
    activeDirectoryId_.clear();
    dupButton_->hide();
    watcher_.setPriority({});
    model_.reload();
    updateHeader();
    updateStatus();
    return;
  }
  model_.album = moved->id;
  activeDirectoryId_ = moved->directoryId;
  watcher_.setPriority(moved->path);
  if (duplicatesVisible()) duplicates_->open(moved->path);
  // The catalog can still be filling under the new name: the first answers may be empty, and an empty list loses the scroll position.
  const int top = gallery_->verticalScrollBar()->value();
  auto settle = std::make_shared<std::function<void(int)>>();
  *settle = [this, settle, top, id = moved->id](int tries) {
    model_.reload([this, settle, top, id, tries] {
      if (model_.album != id) return;
      if (model_.total() == 0 && tries > 0) {
        QTimer::singleShot(250, this, [settle, tries] { (*settle)(tries - 1); });
        return;
      }
      *settle = nullptr;
      gallery_->verticalScrollBar()->setValue(top);
      updateHeader();
      updateStatus();
      refreshFavoriteCounts();
      if (gallery_->nonUniform()) model_.ensureGeometry();
      probeDuplicates();
    }, true, true);
  };
  (*settle)(8);
}
void MainWindow::updateWatcher() {
  QStringList paths;
  paths.reserve(model_.albums.size());
  for (const AlbumInfo &album : model_.albums) paths << album.path;
  const AlbumInfo *active = model_.albumById(model_.album);
  watcher_.setAlbums(paths, active ? active->path : QString());
}
void MainWindow::rebuildTree() {
  QaSpan span("rebuildTree");
  const QString opened = model_.album;
  followActiveAlbum();
  tree_->setAlbums(model_.albums, model_.favoriteCount);
  if (model_.album != opened && !model_.album.isEmpty()) tree_->expandTo(model_.album);
  tree_->setActive(model_.favoritesAlbum ? kFavoritesId : model_.album);
  updateWatcher();
}
void MainWindow::activateAlbum(const QString &id, std::function<void()> done) {
  const bool favorites = id == kFavoritesId;
  if (!favorites && !model_.albumById(id)) {
    if (done) done();
    return;
  }
  tree_->collapseAutoExpandedExcept(id);
  tree_->expandTo(id, !favorites);
  tree_->setActive(id);
  viewer_->closeViewer();
  if (duplicatesVisible()) showDuplicates(false);
  model_.favoritesAlbum = favorites;
  model_.favoritesOnly = false;
  model_.album = favorites ? QString() : id;
  const AlbumInfo *opened = favorites ? nullptr : model_.albumById(id);
  activeDirectoryId_ = opened ? opened->directoryId : QString();
  activeMissingAfter_ = -1;
  gallery_->verticalScrollBar()->setValue(0);
  dupButton_->hide();
  if (const AlbumInfo *album = model_.albumById(id); album && !favorites) watcher_.setPriority(album->path);
  updateHeader();
  model_.reload([this, id, done = std::move(done)] {
    updateStatus();
    // A clicked row keeps focus; Tab goes on to the gallery.
    if (!locked_ && !viewer_->isOpen() && !tree_->hasFocus()) gallery_->setFocus();
    if (done) done();
    if (gallery_->nonUniform()) model_.ensureGeometry();
    probeDuplicates();
  });
}
void MainWindow::probeDuplicates() {
  const QString id = model_.album;
  const AlbumInfo *album = model_.albumById(id);
  if (!album || model_.favoritesAlbum) return;
  // A newer scan cancels the running one, which here is the open review's own.
  if (duplicatesVisible()) {
    if (duplicates_->total() == 0 && !duplicates_->loading() && hashTotal_ > 0 && hashDone_ >= hashTotal_) duplicates_->open(album->path);
    return;
  }
  // Background duplicate probe after activation.
  core_.call(QStringLiteral("find_duplicate_groups"), {{QStringLiteral("dir"), album->path}, {QStringLiteral("offset"), 0}, {QStringLiteral("signature"), QJsonValue()}}, [this, id](const QJsonValue &result, const QString &) {
    if (model_.album == id && !duplicatesVisible()) dupButton_->setVisible(result.toObject().value(QLatin1String("total")).toInt() > 0);
  });
}
void MainWindow::openViewer(int index) {
  if (index < 0 || index >= model_.total() || locked_) return;
  previewItems_.clear();
  const QRect tile = gallery_->tileRect(index);
  QRect origin;
  QImage preview;
  const MediaItem *item = model_.item(index);
  // The tile's picture stands in until the full one is decoded, also while stepping through the album.
  if (item) {
    const QImage *image = pipeline_.get(item->path() + QLatin1Char('#') + QString::number(qRound(tile.width() * devicePixelRatioF())));
    if (!image) image = model_.blur(*item);
    if (image) preview = *image;
  }
  if (!viewer_->isOpen() && item) {
    origin = QRect(gallery_->viewport()->mapTo(host_, tile.topLeft()), tile.size());
    viewer_->setOrigin(origin, gallery_->tileRadius());
    if (Theme::motion(1)) viewer_->setBackdrop(host_->grab());
    viewer_->setFrost(Theme::frostedCopy(host_, host_->rect(), 8));
  }
  viewer_->setPosition(index, model_.total());
  if (item) {
    viewerIndex_ = index;
    viewer_->showItem(*item, origin, preview);
    return;
  }
  // Index outside the retained window: resolve through the catalog.
  core_.call(QStringLiteral("get_viewer_media"), {{QStringLiteral("query"), model_.query()}, {QStringLiteral("index"), index}}, [this, index](const QJsonValue &result, const QString &) {
    viewerIndex_ = index;
    viewer_->setPosition(index, model_.total());
    viewer_->showItem(mediaFromJson(result.toObject()));
  });
}
void MainWindow::stepViewer(int delta) {
  if (delta != 0) viewerStep_ = delta > 0 ? 1 : -1;
  const int next = viewerIndex_ + delta;
  if (!previewItems_.isEmpty()) {
    if (next < 0 || next >= previewItems_.size()) return;
    viewerIndex_ = next;
    viewer_->setPosition(next, previewItems_.size());
    viewer_->showItem(previewItems_[next]);
    return;
  }
  if (next >= 0 && next < model_.total()) openViewer(next);
}
void MainWindow::resolveNeighbors() {
  if (!previewItems_.isEmpty()) return;
  // Both neighbours decode while the current one is on screen, the one in the stepping direction first.
  if (const MediaItem *ahead = model_.item(viewerIndex_ + viewerStep_)) viewer_->prefetch(*ahead);
  if (const MediaItem *behind = model_.item(viewerIndex_ - viewerStep_)) viewer_->prefetch(*behind);
}
QPixmap MainWindow::grabBehindViewer() {
  const qreal ratio = host_->devicePixelRatioF();
  QPixmap out(host_->size() * ratio);
  out.setDevicePixelRatio(ratio);
  out.fill(Qt::transparent);
  QPainter painter(&out);
  host_->render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
  for (QObject *child : host_->children()) {
    auto *widget = qobject_cast<QWidget *>(child);
    if (widget && widget != viewer_ && widget->isVisible() && !widget->isWindow()) widget->render(&painter, widget->pos(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);
  }
  return out;
}
void MainWindow::onViewerClosed() {
  viewerIndex_ = -1;
  previewItems_.clear();
  QTimer::singleShot(400, this, [] { Platform::releaseFreeMemory(); });
  if (duplicatesVisible()) duplicates_->setFocus();
  else gallery_->setFocus();
}
void MainWindow::showDuplicates(bool show, std::function<void()> done) {
  {
    const QSignalBlocker quiet(dupButton_);
    dupButton_->setChecked(show);
  }
  if (show) {
    pipeline_.setDemand({});
    stack_->setCurrentWidget(duplicates_);
    if (const int duration = Theme::motion(200)) {
      duplicates_->setEntrance(0);
      workspaceFade_.stop();
      workspaceFade_.setDuration(duration);
      workspaceFade_.start();
    }
    const AlbumInfo *album = model_.albumById(model_.album);
    updateHeader();
    duplicates_->open(album ? album->path : QString(), std::move(done));
    dupButton_->show();
  } else {
    workspaceFade_.stop();
    duplicates_->setEntrance(1);
    duplicates_->clear();
    stack_->setCurrentWidget(gallery_);
    gallery_->setColumns(gallery_->columns());
    updateHeader();
    if (done) done();
    QTimer::singleShot(400, this, [] { Platform::releaseFreeMemory(); });
  }
}
void MainWindow::scheduleReconcile(const QString &directory) {
  if (model_.root.isEmpty()) return;
  QString relative = QDir(model_.root).relativeFilePath(directory);
  if (relative.endsWith(QLatin1Char('/'))) relative.chop(1);
  if (relative == QLatin1String(".") || relative.isEmpty() || !model_.albumById(relative)) reconcileRoot_ = true;
  else reconcileAlbums_.insert(relative);
  if (!reconcileTimer_.isActive()) reconcileTimer_.start();
}
void MainWindow::reconcileNow() {
  if (!model_.root.isEmpty() && !QFileInfo(model_.root).isDir()) {
    showMissingRoot(model_.root);
    return;
  }
  // Every wakeup is checked against the disk, including changes made during the app's own operations.
  const QSet<QString> albums = reconcileAlbums_;
  const bool root = reconcileRoot_;
  reconcileAlbums_.clear();
  reconcileRoot_ = false;
  if (root) {
    core_.call(QStringLiteral("get_albums_detached"), {{QStringLiteral("rootDir"), model_.root}}, [this](const QJsonValue &value, const QString &) {
      QSet<QString> known, found;
      for (const AlbumInfo &album : model_.albums) known.insert(album.id);
      for (const auto &row : value.toArray()) found.insert(row.toObject().value(QLatin1String("relative_path")).toString());
      if (known == found) return;
      core_.call(QStringLiteral("rescan_inventory"), {{QStringLiteral("rootDir"), model_.root}}, [this](const QJsonValue &, const QString &) { model_.refreshAlbums(); });
    });
  }
  const qint64 now = reconcileClock_.elapsed();
  for (const QString &album : albums) {
    // A busy indexer rewrites its lock files; one check per second per album is enough.
    if (now - lastReconcile_.value(album, -10000) < 1000) {
      reconcileAlbums_.insert(album);
      if (!reconcileTimer_.isActive()) reconcileTimer_.start(1000);
      continue;
    }
    lastReconcile_.insert(album, now);
    core_.call(QStringLiteral("reconcile_album"), {{QStringLiteral("rootDir"), model_.root}, {QStringLiteral("albumId"), album}}, [this, album](const QJsonValue &changed, const QString &error) {
      if (!error.isEmpty() || !changed.toBool()) return;
      // In this order: the refresh may find that the album has moved, and a query by the old name reads nothing.
      model_.refreshAlbums([this, album] {
        if ((album == model_.album && model_.albumById(album)) || model_.favoritesAlbum) model_.reload({}, true);
      });
    });
  }
}
void MainWindow::afterMutation(const QStringList &albums, std::function<void()> done) {
  auto refresh = [this, done = std::move(done)]() mutable {
    model_.refreshAlbums([this, done = std::move(done)]() mutable {
      model_.reload(std::move(done));
      model_.refreshFavoriteCount();
    });
  };
  QStringList touched;
  for (const QString &album : albums)
    if (!album.isEmpty() && model_.albumById(album) && !touched.contains(album)) touched << album;
  if (touched.isEmpty()) return refresh();
  // Rescan the disk, then refresh the list: this settles a scan that overlapped the operation and external changes made during it.
  auto pending = std::make_shared<int>(touched.size());
  for (const QString &album : touched)
    core_.call(QStringLiteral("reconcile_album"), {{QStringLiteral("rootDir"), model_.root}, {QStringLiteral("albumId"), album}}, [pending, refresh](const QJsonValue &, const QString &) mutable {
      if (--*pending == 0) refresh();
    });
}
void MainWindow::moveItems(const QVector<MediaItem> &items, const QString &targetId, std::function<void(QString)> done) {
  const AlbumInfo *target = model_.albumById(targetId);
  if (!target || items.isEmpty()) {
    if (done) done(QStringLiteral("no target or empty"));
    return;
  }
  const int count = items.size();
  const int toast = toasts_->show(ToastHost::Loading, I18n::n("toast.moveMedia.loading", count));
  const QString targetPath = target->path, source = items.first().albumPath, sourceId = items.first().albumId;
  auto batches = std::make_shared<QVector<QVector<MediaItem>>>();
  for (int i = 0; i < count; i += 300) batches->push_back(items.mid(i, 300));
  auto step = std::make_shared<std::function<void(int)>>();
  auto finish = [this, done, toast, count, targetId, sourceId](const QString &error) {
    if (error.isEmpty()) toasts_->update(toast, ToastHost::Success, I18n::n("toast.moveMedia.success", count));
    else toasts_->update(toast, ToastHost::Error, I18n::n("toast.moveMedia.failed", count));
    afterMutation({sourceId, targetId}, [done, error] {
      if (done) done(error);
    });
  };
  *step = [=, this](int n) {
    if (n >= batches->size()) {
      QTimer::singleShot(0, [step] { *step = nullptr; });
      finish({});
      return;
    }
    QJsonArray observed, names;
    for (const MediaItem &m : batches->at(n)) {
      observed.push_back(QJsonObject{{QStringLiteral("path"), m.path()}, {QStringLiteral("revision"), m.revision}});
      names.push_back(m.name);
    }
    core_.call(QStringLiteral("validate_media_revisions"), {{QStringLiteral("observed"), observed}}, [=, this](const QJsonValue &, const QString &validate) {
      if (!validate.isEmpty()) {
        QTimer::singleShot(0, [step] { *step = nullptr; });
        finish(validate);
        return;
      }
      core_.call(QStringLiteral("move_media_batch"), {{QStringLiteral("source"), source}, {QStringLiteral("target"), targetPath}, {QStringLiteral("media"), names}}, [=, this](const QJsonValue &result, const QString &error) {
        if (!error.isEmpty() || !result.toArray().isEmpty()) {
          QTimer::singleShot(0, [step] { *step = nullptr; });
          finish(error.isEmpty() ? QStringLiteral("failed %1").arg(result.toArray().size()) : error);
          return;
        }
        (*step)(n + 1);
      });
    });
  };
  (*step)(0);
}
void MainWindow::deleteItems(const QVector<MediaItem> &items, std::function<void(QString)> done) {
  if (items.isEmpty()) {
    if (done) done(QStringLiteral("empty"));
    return;
  }
  const int count = items.size();
  const int toast = toasts_->show(ToastHost::Loading, I18n::n("toast.deleteMedia.loading", count));
  // Favorites can span albums; each album is one operation.
  QMap<QString, QVector<MediaItem>> byAlbum;
  for (const MediaItem &item : items) byAlbum[item.albumPath].push_back(item);
  auto queue = std::make_shared<QVector<std::pair<QString, QVector<MediaItem>>>>();
  QStringList touched;
  for (auto it = byAlbum.cbegin(); it != byAlbum.cend(); ++it) {
    for (int i = 0; i < it.value().size(); i += 128) queue->push_back({it.key(), it.value().mid(i, 128)});  // Core DELETE_BATCH_LIMIT.
    touched << QDir(model_.root).relativeFilePath(it.key());
  }
  auto step = std::make_shared<std::function<void(int)>>();
  auto finish = [this, done, toast, count, touched](const QString &error) {
    if (error.isEmpty()) toasts_->update(toast, ToastHost::Success, I18n::n("toast.deleteMedia.success", count));
    else toasts_->update(toast, ToastHost::Error, I18n::n("toast.deleteMedia.failed", count));
    afterMutation(touched, [done, error] {
      if (done) done(error);
    });
  };
  *step = [=, this](int n) {
    if (n >= queue->size()) {
      QTimer::singleShot(0, [step] { *step = nullptr; });
      finish({});
      return;
    }
    QJsonArray targets;
    const auto &[album, batch] = queue->at(n);
    for (const MediaItem &m : batch) targets.push_back(QJsonObject{{QStringLiteral("name"), m.name}, {QStringLiteral("revision"), m.revision}});
    core_.call(QStringLiteral("native_delete_media_batch"), {{QStringLiteral("albumPath"), album}, {QStringLiteral("targets"), targets}}, [=, this](const QJsonValue &result, const QString &error) {
      const QString failed = error.isEmpty() ? result.toObject().value(QLatin1String("error")).toString() : error;
      if (!failed.isEmpty()) {
        QTimer::singleShot(0, [step] { *step = nullptr; });
        finish(failed);
        return;
      }
      (*step)(n + 1);
    });
  };
  (*step)(0);
}
void MainWindow::importPaths(const QStringList &paths, const QString &albumId, std::function<void(QString)> done) {
  const AlbumInfo *album = model_.albumById(albumId);
  if (!album) {
    if (done) done(QStringLiteral("album missing"));
    return;
  }
  QStringList media;
  for (const QString &path : paths)
    if (isMediaFile(path) && QFileInfo(path).isFile()) media << path;
  if (media.isEmpty()) {
    showToast(1, t("toast.noFilesAdded"));
    if (done) done(QStringLiteral("no media"));
    return;
  }
  const int count = media.size();
  const int toast = toasts_->show(ToastHost::Loading, I18n::n("toast.addingToAlbum", count));
  const QString directory = album->path;
  auto step = std::make_shared<std::function<void(int)>>();
  auto failed = std::make_shared<int>(0);
  auto failedNames = std::make_shared<QStringList>();
  *step = [=, this](int offset) {
    if (offset >= media.size()) {
      QTimer::singleShot(0, [step] { *step = nullptr; });
      const int added = count - *failed;
      if (*failed == 0) toasts_->update(toast, ToastHost::Success, I18n::n("toast.addedFiles", added));
      else if (added > 0) toasts_->update(toast, ToastHost::Error, t("toast.partialImport", {{"added", QString::number(added)}, {"failed", QString::number(*failed)}, {"files", failedNames->mid(0, 3).join(QStringLiteral(", "))}}));
      else toasts_->update(toast, ToastHost::Error, t("toast.noFilesAdded"));
      afterMutation({albumId}, [done, failed] {
        if (done) done(*failed ? QStringLiteral("failed %1").arg(*failed) : QString());
      });
      return;
    }
    QJsonArray files;
    const QStringList batch = media.mid(offset, 32);
    for (const QString &path : batch) files.push_back(QJsonObject{{QStringLiteral("name"), QFileInfo(path).fileName()}, {QStringLiteral("sourcePath"), path}});
    core_.call(QStringLiteral("add_media_files"), {{QStringLiteral("dir"), directory}, {QStringLiteral("files"), files}}, [=, this](const QJsonValue &result, const QString &error) {
      for (const auto &row : result.toObject().value(QLatin1String("failed")).toArray()) {
        (*failed)++;
        *failedNames << row.toObject().value(QLatin1String("name")).toString();
      }
      if (!error.isEmpty()) {
        qWarning("import: %s", qPrintable(error));
        *failed += batch.size();
      }
      (*step)(offset + batch.size());
    });
  };
  (*step)(0);
}
void MainWindow::refresh(std::function<void()> done) {
  // Album list and current inventory page, without a forced rescan.
  afterMutation({}, std::move(done));
}
void MainWindow::moveAlbum(const QString &albumId, const QString &parentId, std::function<void(QString)> done) {
  core_.call(QStringLiteral("move_album"), {{QStringLiteral("rootDir"), model_.root}, {QStringLiteral("albumId"), albumId}, {QStringLiteral("newParentId"), parentId.isEmpty() ? QJsonValue() : QJsonValue(parentId)}}, [this, done = std::move(done), albumId](const QJsonValue &result, const QString &error) {
    if (!error.isEmpty()) qWarning("move album: %s", qPrintable(error));
    followRenamedAlbum(albumId, error.isEmpty() ? result.toObject().value(QLatin1String("newRelativePath")).toString() : QString(), error, std::move(done));
  });
}
void MainWindow::renameAlbum(const QString &albumId, const QString &name, std::function<void(QString)> done) {
  core_.call(QStringLiteral("rename_album"), {{QStringLiteral("rootDir"), model_.root}, {QStringLiteral("albumId"), albumId}, {QStringLiteral("newName"), name}}, [this, albumId, done = std::move(done)](const QJsonValue &result, const QString &error) {
    // The core trims the name and replaces path separators, so the new id comes from its answer.
    followRenamedAlbum(albumId, error.isEmpty() ? result.toObject().value(QLatin1String("newRelativePath")).toString() : QString(), error, std::move(done));
  });
}
void MainWindow::followRenamedAlbum(const QString &from, const QString &to, const QString &error, std::function<void(QString)> done) {
  // The open album may be the renamed one or lie inside it.
  QString active;
  if (!to.isEmpty() && model_.album == from) active = to;
  else if (!to.isEmpty() && model_.album.startsWith(from + QLatin1Char('/'))) active = to + model_.album.mid(from.size());
  if (!active.isEmpty()) model_.album = active;
  model_.refreshAlbums([this, done = std::move(done), error, active] {
    if (!active.isEmpty() && model_.album == active) {
      tree_->expandTo(active);
      tree_->setActive(active);
      model_.reload();
    }
    if (done) done(error);
  });
}
void MainWindow::deleteAlbum(const QString &albumId, std::function<void(QString)> done) {
  core_.call(QStringLiteral("delete_album"), {{QStringLiteral("rootDir"), model_.root}, {QStringLiteral("albumId"), albumId}}, [this, albumId, done = std::move(done)](const QJsonValue &, const QString &error) {
    if (error.isEmpty() && (model_.album == albumId || model_.album.startsWith(albumId + QLatin1Char('/')))) {
      model_.album.clear();
      model_.reload();
      tree_->setActive({});
      updateHeader();
      updateStatus();
    }
    model_.refreshAlbums([done, error] {
      if (done) done(error);
    });
  });
}
void MainWindow::createAlbum(const QString &parentPath, const QString &name, std::function<void()> done) {
  // This write happens locally, before the core gateway can validate a root.
  if (!rootAllowed(parentPath)) {
    if (done) done();
    return;
  }
  const QString cleaned = name.trimmed();
  if (cleaned.isEmpty() || cleaned.contains(QLatin1Char('/')) || cleaned.contains(QLatin1Char('\\')) || cleaned.startsWith(QLatin1Char('.'))) {
    showToast(1, t("toast.albumNameInvalid"));
    if (done) done();
    return;
  }
  if (QFileInfo::exists(QDir(parentPath).filePath(cleaned))) {
    showToast(1, t("toast.albumExists", {{"album", cleaned}}));
    if (done) done();
    return;
  }
  if (!QDir(parentPath).mkdir(cleaned)) {
    showToast(1, t("toast.albumCreateFailed"));
    if (done) done();
    return;
  }
  // Only the new album changed; a library-wide rescan would re-read every album and re-queue its preload.
  // A parent outside the open library (another spelling of the root, a decoy) keeps the library-wide scan.
  QJsonObject scan{{QStringLiteral("rootDir"), model_.root}};
  const QString created = QDir(model_.root).relativeFilePath(QDir(parentPath).filePath(cleaned));
  if (!created.isEmpty() && !QDir::isAbsolutePath(created) && created != QLatin1String("..") && !created.startsWith(QLatin1String("../"))) scan.insert(QStringLiteral("albumId"), created);
  lastAlbumScan_ = scan.contains(QStringLiteral("albumId")) ? QStringLiteral("album:") + created : QStringLiteral("library");
  core_.call(QStringLiteral("rescan_inventory"), scan, [this, done = std::move(done)](const QJsonValue &, const QString &) {
    model_.refreshAlbums([done] {
      if (done) done();
    });
  });
}
void MainWindow::setFavorite(const MediaItem &item, bool favorite, std::function<void(QString)> done) {
  core_.call(QStringLiteral("set_media_favorite"), {{QStringLiteral("path"), item.path()}, {QStringLiteral("favorite"), favorite}}, [this, item, favorite, done = std::move(done)](const QJsonValue &, const QString &error) {
    if (error.isEmpty()) {
      if (viewer_->isOpen() && viewer_->currentPath() == item.path()) viewer_->setFavorite(favorite);
      favoriteCounts_[item.albumId] = qMax(0, favoriteCounts_.value(item.albumId) + (favorite ? 1 : -1));
      updateHeader();
    }
    model_.refreshFavoriteCount();
    model_.reload([done, error] {
      if (done) done(error);
    }, true, !model_.favoritesOnly && !model_.favoritesAlbum);
  });
}
void MainWindow::setItemsDate(const QVector<MediaItem> &items, qint64 timestamp, std::function<void(QString)> done) {
  QMap<QString, QJsonArray> byAlbum;
  for (const MediaItem &item : items) byAlbum[item.albumPath].push_back(item.name);
  auto left = std::make_shared<int>(byAlbum.size());
  auto firstError = std::make_shared<QString>();
  for (auto it = byAlbum.cbegin(); it != byAlbum.cend(); ++it) {
    core_.call(QStringLiteral("set_media_timestamp"), {{QStringLiteral("albumPath"), it.key()}, {QStringLiteral("names"), it.value()}, {QStringLiteral("timestamp"), double(timestamp)}}, [this, left, firstError, done](const QJsonValue &, const QString &error) {
      if (!error.isEmpty() && firstError->isEmpty()) *firstError = error;
      if (--*left) return;
      if (firstError->isEmpty()) showToast(0, t("toast.dateUpdated"));
      else showToast(1, *firstError);
      model_.reload([done, firstError] {
        if (done) done(*firstError);
      }, true);
    });
  }
}
void MainWindow::copyItems(const QVector<MediaItem> &items) {
  if (items.isEmpty()) return;
  if (inProtectedRoot()) {
    confirmExport([this, items] {
      const auto urls = exportUrls(items);
      if (!urls || urls->isEmpty()) return;
      auto *mime = new QMimeData;
      mime->setUrls(*urls);
      QGuiApplication::clipboard()->setMimeData(mime);
      showToast(0, I18n::n("toast.filesCopied", urls->size()));
    });
    return;
  }
  auto *mime = new QMimeData;
  QList<QUrl> urls;
  QStringList paths;
  for (const MediaItem &item : items) {
    urls << QUrl::fromLocalFile(item.path());
    paths << item.path();
  }
  mime->setUrls(urls);
  mime->setText(paths.join(QLatin1Char('\n')));
  QGuiApplication::clipboard()->setMimeData(mime);
  showToast(0, I18n::n("toast.filesCopied", items.size()));
}
void MainWindow::pasteFromClipboard() {
  if (model_.album.isEmpty() || model_.favoritesAlbum) return;
  const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
  if (!mime) return;
  QStringList paths;
  for (const QUrl &url : mime->urls())
    if (url.isLocalFile() && isMediaFile(url.toLocalFile())) paths << url.toLocalFile();
  if (paths.isEmpty() && mime->hasImage()) {
    const QImage image = qvariant_cast<QImage>(mime->imageData());
    const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("room237-paste"));
    QDir().mkpath(directory);
    const QString file = QDir(directory).filePath(QStringLiteral("pasted-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    if (image.save(file, "PNG")) {
      importPaths({file}, model_.album, [file](const QString &) { QFile::remove(file); });
      return;
    }
  }
  if (!paths.isEmpty()) importPaths(paths, model_.album);
}
void MainWindow::selectedOrItem(int index, std::function<void(QVector<MediaItem>)> done) {
  const MediaItem *item = model_.item(index);
  if (gallery_->selectionCount() > 0 && gallery_->isSelected(index)) return gallery_->collectSelection(std::move(done));
  if (gallery_->selectionCount() > 0) return gallery_->collectSelection(std::move(done));
  done(item ? QVector<MediaItem>{*item} : QVector<MediaItem>{});
}
void MainWindow::showMediaMenu(int index, QPoint global) {
  const MediaItem *item = model_.item(index);
  if (!item) return;
  const MediaItem media = *item;
  const int selected = gallery_->selectionCount();
  const int count = qMax(1, selected);
  QVector<MenuItem> menu;
  menu.push_back({"copy", selected > 1 ? I18n::n("contextMenu.copyFiles", selected) : t("contextMenu.copyFile"), [this, index] { selectedOrItem(index, [this](const QVector<MediaItem> &items) { copyItems(items); }); }});
  if (selected <= 1) {
    menu.push_back({Platform::fileManagerIcon(), t("contextMenu.revealIn", {{"fileManager", Platform::fileManagerName()}}), [media] { Platform::revealInFileManager(media.path()); }});
    menu.push_back({"link", t("contextMenu.copyPath"), [this, media] {
      QGuiApplication::clipboard()->setText(media.path());
      showToast(0, t("toast.pathCopied"));
    }});
  }
  MenuItem date{"calendar-clock", count > 1 ? I18n::n("contextMenu.changeDateFor", count) : t("contextMenu.changeDate")};
  date.separatorBefore = true;
  date.panel = [this, media, index](QWidget *host) -> Popover * {
    const double seconds = media.displayDate();
    return new DatePanel(host, seconds > 0 ? QDateTime::fromSecsSinceEpoch(qint64(seconds)) : QDateTime::currentDateTime(), [this, index](const QDateTime &when) {
      selectedOrItem(index, [this, when](const QVector<MediaItem> &items) { setItemsDate(items, when.toSecsSinceEpoch()); });
    });
  };
  menu.push_back(date);
  MenuItem move{"folder-open", count > 1 ? I18n::n("contextMenu.moveItems", count) : t("contextMenu.moveToAlbum")};
  move.submenu = [this, media, index]() {
    QVector<MenuItem> albums;
    bool any = false;
    for (int i = 0; i < tree_->rowCount(); i++) {
      const QString id = tree_->rowId(i);
      const AlbumInfo *album = model_.albumById(id);
      if (!album || id == kFavoritesId) continue;
      MenuItem entry{"folder", album->name, [this, id, index] { selectedOrItem(index, [this, id](const QVector<MediaItem> &items) { moveItems(items, id); }); }, id != media.albumId};
      entry.indent = id.count(QLatin1Char('/'));
      any |= id != media.albumId;
      albums.push_back(entry);
    }
    if (!any) return QVector<MenuItem>{{{}, t("contextMenu.noOtherAlbums"), {}, false}};
    return albums;
  };
  menu.push_back(move);
  MenuItem remove{"trash", count > 1 ? I18n::n("contextMenu.deleteCount", count) : t("contextMenu.delete")};
  remove.danger = true;
  remove.separatorBefore = true;
  remove.panel = [this, index, count](QWidget *host) -> Popover * {
    return new DeleteSubmenu(host, I18n::n("contextMenu.deleteItems", count), t("contextMenu.deleteWarning"), t("contextMenu.delete"), [this, index, host] {
      for (Menu *open : host->findChildren<Menu *>(Qt::FindDirectChildrenOnly)) open->closeNow();
      selectedOrItem(index, [this](const QVector<MediaItem> &items) { deleteItems(items); });
    });
  };
  menu.push_back(remove);
  Menu::popup(host_, menu, host_->mapFromGlobal(global));
}
void MainWindow::showAlbumMenu(const QString &albumId, QPoint global) {
  const AlbumInfo *found = model_.albumById(albumId);
  if (!found) return;
  const AlbumInfo album = *found;
  const QPoint at = host_->mapFromGlobal(global);
  const QRect anchor(at, QSize(1, 1));
  // "Move to" and "Delete" open in place of the menu.
  QVector<MenuItem> menu;
  menu.push_back({"plus", t("album.newSubAlbum"), [this, album, anchor] {
    auto holder = std::make_shared<QPointer<TextPrompt>>();
    auto *dialog = new TextPrompt(host_, t("album.newSubAlbumIn", {{"album", album.name}}), t("common.create"), {}, [this, holder, album](const QString &name) {
      if (*holder) (*holder)->closeNow();
      createAlbum(album.path, name, [this, album] { tree_->setExpanded(album.id, true); });
    });
    *holder = dialog;
    dialog->openAt(anchor, Qt::RightEdge, Qt::AlignTop);
  }});
  menu.push_back({"pencil", t("album.renameAction"), [this, album, anchor] {
    auto holder = std::make_shared<QPointer<TextPrompt>>();
    auto *dialog = new TextPrompt(host_, t("album.rename"), t("common.save"), album.name, [this, holder, album](const QString &name) {
      if (*holder) (*holder)->closeNow();
      renameAlbum(album.id, name, [this](const QString &error) {
        if (!error.isEmpty()) showToast(1, error);
      });
    });
    *holder = dialog;
    dialog->openAt(anchor, Qt::RightEdge, Qt::AlignTop);
  }});
  menu.push_back({Platform::fileManagerIcon(), t("contextMenu.revealIn", {{"fileManager", Platform::fileManagerName()}}), [album] { Platform::revealInFileManager(album.path); }});
  menu.push_back({"link", t("contextMenu.copyPath"), [this, album] {
    QGuiApplication::clipboard()->setText(album.path);
    showToast(0, t("toast.pathCopied"));
  }});
  MenuItem move{"folder-open", t("album.moveTo"), [this, album, at, global] {
    QVector<MenuItem> targets;
    targets.push_back({"chevron-left", t("album.moveTo"), [this, album, global] { showAlbumMenu(album.id, global); }});
    MenuItem root{"folder", t("album.libraryRoot"), [this, album] {
      moveAlbum(album.id, {}, [this, album](const QString &error) { showToast(error.isEmpty() ? 0 : 1, error.isEmpty() ? t("toast.albumMoved", {{"album", album.name}}) : t("toast.albumMoveFailed")); });
    }, !album.parent.isEmpty()};
    root.separatorBefore = true;
    targets.push_back(root);
    for (int i = 0; i < tree_->rowCount(); i++) {
      const QString id = tree_->rowId(i);
      const AlbumInfo *target = model_.albumById(id);
      if (!target || id == album.id || id.startsWith(album.id + QLatin1Char('/'))) continue;
      MenuItem entry{"folder", target->name, [this, album, id] {
        moveAlbum(album.id, id, [this, album](const QString &error) { showToast(error.isEmpty() ? 0 : 1, error.isEmpty() ? t("toast.albumMoved", {{"album", album.name}}) : t("toast.albumMoveFailed")); });
      }, id != album.parent};
      entry.indent = id.count(QLatin1Char('/'));
      targets.push_back(entry);
    }
    Menu::popup(host_, targets, at);
  }};
  move.chevron = true;
  menu.push_back(move);
  MenuItem remove{"trash", t("common.delete") + QStringLiteral("…"), [this, album, anchor] {
    auto *panel = new ConfirmPanel(host_, t("album.deleteNamed", {{"album", album.name}}), t("album.deleteDescription"), t("common.delete"), [this, album] {
      deleteAlbum(album.id, [this](const QString &error) {
        if (!error.isEmpty()) showToast(1, error);
      });
    });
    panel->setActionIcon("trash");
    panel->openAt(anchor, Qt::RightEdge, Qt::AlignTop);
  }};
  remove.danger = true;
  remove.separatorBefore = true;
  menu.push_back(remove);
  Menu::popup(host_, menu, at);
}
SettingsCallbacks MainWindow::settingsCallbacks() {
  SettingsCallbacks c;
  c.version = QStringLiteral(ROOM237_VERSION);
  c.changeRoot = [this] { pickRootDirectory(QStringLiteral("root")); };
  c.pickDecoy = [this] { pickRootDirectory(QStringLiteral("decoy")); };
  c.clearDecoy = [this] { clearDecoy(); };
  c.openAdvanced = [this] { openAdvancedSettings(); };
  c.checkUpdates = [this] { updater_->checkNow(true); };
  c.setContentProtected = [this](bool on) {
    ui_.contentProtected = on;
    saveTimer_.start();
    updatePrivacySurface();
  };
  c.setAutoUpdate = [this](bool on) {
    ui_.autoUpdate = on;
    updater_->setAutomatic(on && !QaFence::enabled());
    saveTimer_.start();
  };
  c.setScroll60 = [this](bool on) {
    ui_.scroll60 = on;
    gallery_->setFrameCap(on ? 16 : 0);
    saveTimer_.start();
  };
  c.setLanguage = [this](const QString &language) { applyLanguage(language); };
  c.language = [this] { return ui_.language; };
  c.decoyRoot = [this] { return ui_.decoyRoot; };
  c.privacyEnabled = [this] { return privacyEnabled_; };
  c.contentProtected = [this] { return ui_.contentProtected; };
  c.autoUpdate = [this] { return ui_.autoUpdate; };
  c.scroll60 = [this] { return ui_.scroll60; };
  c.showRootControls = !displayDecoy_ && !locked_;
  c.vaultState = [this]() -> QString {
    if (displayDecoy_ || model_.root.isEmpty()) return QStringLiteral("none");
    const QJsonObject status = vaultStatus(model_.root);
    if (status.value(QLatin1String("nested")).toBool()) return QStringLiteral("nested");
    if (!status.value(QLatin1String("vault")).toBool()) return QStringLiteral("none");
    return status.value(QLatin1String("state")).toString();
  };
  c.encryptRoot = [this] { startHardeningSetup(); };
  c.decryptRoot = [this] { removeEncryption(); };
  c.replaceFactor = [this] { replaceFactor(); };
  return c;
}
void MainWindow::openSettings() {
  auto *panel = new SettingsPanel(host_, settingsCallbacks());
  settingsButton_->setHeld(true);
  connect(panel, &Popover::closed, settingsButton_, [this] { settingsButton_->setHeld(false); });
  panel->setObjectName(QStringLiteral("settings"));
  QWidget *anchor = settingsButton_;
  for (HeroScreen *screen : {directoryScreen_, confirmScreen_})
    if (screen->isVisible()) anchor = screen->findChild<IconButton *>(QStringLiteral("cornerSettings"));
  panel->setTrigger(anchor);
  panel->openAt(hostRect(host_, anchor), anchor == settingsButton_ ? Qt::TopEdge : Qt::BottomEdge, anchor == settingsButton_ ? Qt::AlignLeft : Qt::AlignRight);
  const auto spin = [this](qreal target) {
    gearSpin_.stop();
    gearSpin_.setDuration(Theme::motion(300));
    gearSpin_.setStartValue(gearSpin_.currentValue().isValid() ? gearSpin_.currentValue() : QVariant(0.0));
    gearSpin_.setEndValue(target);
    gearSpin_.start();
  };
  spin(90);
  connect(panel, &Popover::closed, this, [spin] { spin(0); });
}
void MainWindow::openAdvancedSettings() {
  auto *dialog = new AdvancedDialog(host_, &core_, settingsCallbacks(), [this](const QJsonObject &settings) { onSettings(settings); }, [this](int kind, const QString &text) { showToast(kind, text); });
  dialog->setObjectName(QStringLiteral("advanced"));
  // An anchor half the dialog above the middle leaves it centred.
  const QSize size = dialog->panelRect().size();
  dialog->openAt(QRect(host_->width() / 2, (host_->height() - size.height()) / 2 - 7, 1, 1), Qt::BottomEdge, Qt::AlignHCenter);
}
void MainWindow::setLocked(bool locked) {
  if (locked && inProtectedRoot()) {
    lockVault();
    return;
  }
  if (locked == locked_) return;
  locked_ = locked;
  if (locked) {
    Popover::closeAll(host_);
    lockFade_.stop();
    lockScreen_->setGraphicsEffect(nullptr);
    lockScreen_->setFrost(Theme::frostedCopy(host_, host_->rect(), 16));
    lockScreen_->setGeometry(host_->rect());
    lockScreen_->show();
    lockScreen_->raise();
    toasts_->setHiddenForPrivacy(true);
  } else {
    // Leaving takes 300ms; the frosted copy holds no readable detail, and the app underneath is back.
    if (const int duration = Theme::motion(300)) {
      lockScreen_->setGraphicsEffect(new QGraphicsOpacityEffect(lockScreen_));
      lockFade_.setDuration(duration);
      lockFade_.start();
    } else {
      lockScreen_->hide();
    }
    toasts_->setHiddenForPrivacy(unfocused_);
    if (lockscreenEnabled_) inactivity_.start();
    (viewer_->isOpen() ? static_cast<QWidget *>(viewer_) : gallery_)->setFocus();
  }
  gallery_->setBlocked(locked || unfocused_);
  tree_->setBlocked(locked || unfocused_);
  windowStateTimer_.start();
}
void MainWindow::setUnfocused(bool unfocused) {
  if (unfocused == unfocused_) return;
  unfocused_ = unfocused;
  if (unfocused) {
    Popover::closeAll(host_);
    shield_->showFrom(host_);
  } else {
    shield_->hide();
  }
  toasts_->setHiddenForPrivacy(unfocused || locked_);
  gallery_->setBlocked(unfocused || locked_);
  tree_->setBlocked(unfocused || locked_);
  windowStateTimer_.start();
}
void MainWindow::resetRoot() {
  ui_.rootDir.clear();
  saveUiState();
  allowOpen_ = false;
  model_.setRoot(QString());
  watcher_.stop();
  showStartScreens();
}
bool MainWindow::galleryKeysAllowed() const {
  if (locked_ || unfocused_ || viewer_->isOpen() || duplicatesVisible() || !allowOpen_) return false;
  for (Popover *popover : host_->findChildren<Popover *>(Qt::FindDirectChildrenOnly))
    if (popover->isOpen()) return false;
  return true;
}
void MainWindow::setDebuggerVisible(bool visible) {
  if (visible && !debugger_) {
    // These strings stay in English in every language.
    auto *panel = new HeroScreen(host_, QStringLiteral("debugger"), true);
    panel->setSubtitleStyle(14, "rgba(255,255,255,128)");
    panel->setTexts(QStringLiteral("This is a debug menu"), QStringLiteral("Please, be careful with the options here, they are meant for debugging purposes and may cause unexpected behavior."));
    const auto action = [panel](const QString &text, const QByteArray &icon, TextButton::Kind kind) {
      auto *button = new TextButton(text, kind);
      button->setIcon(icon);
      panel->addVerticalButton(button);
      return button;
    };
    const auto call = [this](const QString &method) {
      return [this, method](TextButton *button) {
        button->setBusy(true);
        core_.call(method, {{QStringLiteral("rootDir"), effectiveRoot()}}, [this, button](const QJsonValue &, const QString &error) {
          button->setBusy(false);
          showToast(error.isEmpty() ? 0 : 1, error.isEmpty() ? t("common.done") : error);
          refresh();
        });
      };
    };
    auto *thumbs = action(QStringLiteral("Rebuild Thumbnails"), "layout-grid", TextButton::Primary);
    connect(thumbs, &QAbstractButton::clicked, this, [thumbs, run = call(QStringLiteral("rebuild_thumbnails"))] { run(thumbs); });
    auto *meta = action(QStringLiteral("Rebuild Metadata"), "book", TextButton::Primary);
    connect(meta, &QAbstractButton::clicked, this, [meta, run = call(QStringLiteral("rebuild_metadata"))] { run(meta); });
    auto *logger = action(QStringLiteral("Open Logger"), "file-text", TextButton::Primary);
    connect(logger, &QAbstractButton::clicked, this, [this, logger] {
      const bool show = !logger_->isVisible();
      logger_->setVisible(show);
      if (show) logger_->raise();
      logger->setText(show ? QStringLiteral("Close Logger") : QStringLiteral("Open Logger"));
    });
    auto *reset = action(QStringLiteral("Reset Duplicates"), "refresh", TextButton::Outline);
    connect(reset, &QAbstractButton::clicked, this, [reset, run = call(QStringLiteral("reset_duplicates"))] { run(reset); });
    auto *clear = action(QStringLiteral("Clear room237 artifacts"), "trash", TextButton::Danger);
    connect(clear, &QAbstractButton::clicked, this, [this, clear] {
      clear->setBusy(true);
      core_.call(QStringLiteral("clear_room237_artifacts"), {{QStringLiteral("rootDir"), effectiveRoot()}}, [this](const QJsonValue &, const QString &error) {
        if (!error.isEmpty()) return showToast(1, error);
        setDebuggerVisible(false);
        resetRoot();
      });
    });
    debugger_ = panel;
  }
  if (visible && debugger_) static_cast<HeroScreen *>(debugger_)->setFrost(Theme::frostedCopy(host_, host_->rect(), 8));
  if (debugger_) {
    debugger_->setGeometry(host_->rect());
    debugger_->setVisible(visible);
    if (visible) debugger_->raise();
  }
}
void MainWindow::mousePressEvent(QMouseEvent *event) {
  // Empty space in the header moves the window, the standard behaviour of a unified title bar.
  if (event->button() == Qt::LeftButton && event->position().y() < qMax(36, Platform::titleBarInset()) + 28 && windowHandle()) {
    windowHandle()->startSystemMove();
    return;
  }
  QMainWindow::mousePressEvent(event);
}
void MainWindow::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->position().y() < qMax(36, Platform::titleBarInset()) + 28) isMaximized() ? showNormal() : showMaximized();
  else QMainWindow::mouseDoubleClickEvent(event);
}
void MainWindow::resizeEvent(QResizeEvent *event) {
  QMainWindow::resizeEvent(event);
  const QRect area = host_->rect();
  // Sizes in percent; the card leaves a margin on three sides, the handle sits in the gap after it.
  const int panel = qRound(area.width() * ui_.sidebarFraction);
  sidebarPanel_->setGeometry(0, 0, panel, area.height());
  sidebar_->setGeometry(8, 8, panel - 8, area.height() - 16);
  sidebarHandle_->setGeometry(panel + 2, 0, 8, area.height());
  rightPane_->setGeometry(panel + 9, 0, area.width() - panel - 9, area.height());
  for (QWidget *screen : {static_cast<QWidget *>(startupScreen_), static_cast<QWidget *>(directoryScreen_), static_cast<QWidget *>(confirmScreen_), static_cast<QWidget *>(lockScreen_), static_cast<QWidget *>(vaultScreen_), static_cast<QWidget *>(shield_), static_cast<QWidget *>(preload_), debugger_})
    if (screen) screen->setGeometry(area);
  for (HeroScreen *screen : {directoryScreen_, confirmScreen_})
    // Centred 27 points from the corner, mirroring the window buttons on the left.
    if (auto *gear = screen->findChild<IconButton *>(QStringLiteral("cornerSettings"))) gear->move(area.width() - 27 - 16, 27 - 16);
  if (viewer_->isVisible()) viewer_->setGeometry(area);
  logger_->move(area.width() - logger_->width() - 16, area.height() - logger_->height() - 16);
  toasts_->hostResized();
  sidebarHandle_->raise();
  if (QGuiApplication::platformName() != QLatin1String("offscreen")) Platform::layoutWindowControls(this);
}
void MainWindow::moveEvent(QMoveEvent *event) {
  QMainWindow::moveEvent(event);
  viewer_->windowMoved();
}
void MainWindow::closeEvent(QCloseEvent *event) {
  closing_ = true;
  saveUiState();
  SecureMedia::clearExports();
  core::vault_ops::lockAll();
  QMainWindow::closeEvent(event);
}
void MainWindow::changeEvent(QEvent *event) {
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::ActivationChange) {
    if (privacyEnabled_) setUnfocused(!isActiveWindow());
  } else if (event->type() == QEvent::WindowStateChange) {
    windowStateTimer_.start();
    if (windowHandle() && QGuiApplication::platformName() != QLatin1String("offscreen")) Platform::layoutWindowControls(this);
  }
}
bool MainWindow::eventFilter(QObject *object, QEvent *event) {
  if (object == sidebarHandle_) {
    if (event->type() == QEvent::MouseButtonPress) {
      resizingSidebar_ = true;
      return true;
    }
    if (event->type() == QEvent::MouseMove && resizingSidebar_) {
      const int x = host_->mapFromGlobal(static_cast<QMouseEvent *>(event)->globalPosition().toPoint()).x();
      ui_.sidebarFraction = qBound(0.16, double(x) / host_->width(), 0.24);
      QResizeEvent resize(size(), size());
      resizeEvent(&resize);
      return true;
    }
    if (event->type() == QEvent::MouseButtonRelease) {
      resizingSidebar_ = false;
      saveTimer_.start();
      return true;
    }
    return false;
  }
  if ((lockscreenEnabled_ || inProtectedRoot()) && !locked_ && (event->type() == QEvent::MouseMove || event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress)) inactivity_.start();
  if (event->type() != QEvent::KeyPress || !object->isWidgetType()) return QMainWindow::eventFilter(object, event);
  auto *key = static_cast<QKeyEvent *>(event);
  auto *widget = static_cast<QWidget *>(object);
  if (widget->window() != this) return false;
  // An ignored key travels up the parents as the same event; the sequence below must see it once.
  if (key->timestamp() == lastKeyStamp_ && key->key() == lastKey_) return false;
  lastKeyStamp_ = key->timestamp();
  lastKey_ = key->key();
  // While the processing screen is up, keys reach only its own controls; the lock and unlock screens stay above it.
  const bool lockKey = (lockscreenEnabled_ || inProtectedRoot()) && keyIs(key, QLatin1Char('l')) && !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier));
  if (preload_->isVisible() && !locked_ && !vaultScreen_->isVisible() && !preload_->isAncestorOf(widget) && !lockKey) return true;
  if (vaultScreen_->isVisible()) {
    // Command-U opens the decoy gallery; typing goes to the fields.
    if (keyIs(key, QLatin1Char('u')) && key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier) && !ui_.decoyRoot.isEmpty()) {
      onVaultUnlocked(true);
      return true;
    }
    return false;
  }
  if (locked_) {
    if (keyIs(key, QLatin1Char('u'))) {
      if (key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier) && !ui_.decoyRoot.isEmpty()) {
        if (!switchDecoy()) return true;
      }
      setLocked(false);
    }
    return true;
  }
  if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) || qobject_cast<QAbstractSpinBox *>(QApplication::focusWidget())) return false;
  if (!allowOpen_ || unfocused_) return false;
  // Hidden debug entry: type "venari".
  if (keyClock_.isValid() && keyClock_.elapsed() > 2000) keyBuffer_.clear();
  keyClock_.start();
  keyBuffer_ = (keyBuffer_ + key->text().toLower()).right(32);
  if (keyBuffer_.contains(QLatin1String("venari"))) {
    keyBuffer_.clear();
    setDebuggerVisible(true);
    return true;
  }
  if (debugger_ && debugger_->isVisible() && key->key() == Qt::Key_Escape) {
    setDebuggerVisible(false);
    return true;
  }
  if ((lockscreenEnabled_ || inProtectedRoot()) && keyIs(key, QLatin1Char('l')) && !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier))) {
    setLocked(true);
    return true;
  }
  return false;
}
// --- Security hardening ----------------------------------------------------------------------------------------------
QJsonObject MainWindow::vaultStatus(const QString &root) const {
  if (root.isEmpty()) return {};
  try {
    return core::vault_ops::status(root);
  } catch (const std::exception &) {
    return {};
  }
}
void MainWindow::showVaultUnlock(const QString &root) {
  Popover::closeAll(host_);
  viewer_->closeViewer();
  if (duplicatesVisible()) showDuplicates(false);
  confirmScreen_->hide();
  directoryScreen_->hide();
  vaultScreen_->start(root, vaultStatus(root));
  vaultScreen_->setGeometry(host_->rect());
  vaultScreen_->show();
  vaultScreen_->raise();
  if (shield_->isVisible()) shield_->raise();
  updatePrivacySurface();
}
void MainWindow::onVaultUnlocked(bool decoy) {
  const QString root = vaultScreen_->root();
  if (decoy) {
    // The decoy password shows the decoy gallery as if it were the room; the real root stays locked.
    if (ui_.decoyRoot.isEmpty() || !rootAllowed(ui_.decoyRoot)) return;
    vaultScreen_->hide();
    displayDecoy_ = true;
    allowOpen_ = true;
    openRoot(ui_.decoyRoot);
    updatePrivacySurface();
    return;
  }
  displayDecoy_ = false;
  allowOpen_ = true;
  const bool recovered = vaultScreen_->usedRecovery();
  vaultScreen_->hide();
  openRoot(root);
  // The recovery key stands in for a factor that no longer works here.
  if (recovered) QTimer::singleShot(0, this, &MainWindow::replaceFactor);
}
void MainWindow::lockVault() {
  const QString root = vaultRoot_;
  core_.call(QStringLiteral("vault_lock"), {{QStringLiteral("root"), root}});
  Popover::closeAll(host_);
  viewer_->closeViewer();
  if (duplicatesVisible()) showDuplicates(false);
  watcher_.stop();
  // Nothing decrypted stays in memory or on disk: previews, the open album and copies for other apps.
  pipeline_.clear();
  model_.setRoot(QString());
  model_.album.clear();
  model_.albums.clear();
  model_.reload();
  rebuildTree();
  tree_->dropIcons();
  SecureMedia::clearExports();
  exportAllowed_ = false;
  vaultRoot_.clear();
  allowOpen_ = false;
  showVaultUnlock(root);
}
void MainWindow::offerEncryption() {
  const QString root = model_.root;
  if (!hardening_ || root.isEmpty() || displayDecoy_ || !albumsReady_ || preload_->isVisible() || offeredRoots_.contains(root) || ui_.hardeningNeverAsk.contains(root)) return;
  const QJsonObject status = vaultStatus(root);
  if (status.value(QLatin1String("vault")).toBool() || status.value(QLatin1String("nested")).toBool()) return;
  offeredRoots_.insert(root);
  auto *offer = new EncryptOffer(host_, QFileInfo(root).fileName(), [this] { startHardeningSetup(); }, {}, [this, root] {
    ui_.hardeningNeverAsk << root;
    saveTimer_.start();
  });
  offer->setObjectName(QStringLiteral("encryptOffer"));
  openCentered(offer);
}
void MainWindow::startHardeningSetup() {
  const QString root = model_.root;
  if (root.isEmpty() || displayDecoy_) return;
  ui_.hardeningNeverAsk.removeAll(root);
  // The decoy password is offered only when there is a decoy gallery to show.
  auto *setup = new HardeningSetup(host_, &core_, root, privacyEnabled_ && !ui_.decoyRoot.isEmpty(), [this, root] {
    showToast(0, t("security.toast.encrypting"));
    if (model_.root == root) openRoot(root);
  });
  setup->setObjectName(QStringLiteral("hardeningSetup"));
  openCentered(setup);
}
void MainWindow::removeEncryption() {
  const QString root = vaultRoot_;
  if (root.isEmpty()) return;
  auto *panel = new RemoveEncryption(host_, &core_, root, [this] { showToast(0, t("security.toast.decrypting")); });
  panel->setObjectName(QStringLiteral("removeEncryption"));
  openCentered(panel);
}
void MainWindow::replaceFactor() {
  if (vaultRoot_.isEmpty()) return;
  auto *panel = new ReplaceFactor(host_, &core_, vaultRoot_, [this](const QString &) { showToast(0, t("security.toast.factorAdded")); });
  panel->setObjectName(QStringLiteral("replaceFactor"));
  openCentered(panel);
}
void MainWindow::onVaultProgress(const QJsonObject &o) {
  const QString root = o.value(QLatin1String("root")).toString(), phase = o.value(QLatin1String("phase")).toString();
  const int done = o.value(QLatin1String("done")).toInt(), total = o.value(QLatin1String("total")).toInt();
  if (root != model_.root) return;
  const bool running = phase == QLatin1String("encrypting") || phase == QLatin1String("decrypting");
  if (running && total > 0) {
    preload_->setVault(phase == QLatin1String("encrypting") ? t("vault.overlay.encrypting") : t("vault.overlay.decrypting"), done, total);
    if (!vaultRefresh_.isActive()) vaultRefresh_.start();
    return;
  }
  preload_->finishVault();
  vaultRefresh_.stop();
  if (phase == QLatin1String("incomplete")) {
    showToast(1, t("security.toast.incomplete", {{"error", o.value(QLatin1String("error")).toString()}}));
    return;
  }
  if (phase == QLatin1String("encrypted") && total > 0) showToast(0, t("security.toast.encrypted"));
  if (phase == QLatin1String("decrypted")) showToast(0, t("security.toast.decrypted"));
  // New names and previews: the room lists itself again.
  if (phase == QLatin1String("decrypted") || total > 0) openRoot(root);
}
void MainWindow::confirmExport(std::function<void()> proceed) {
  if (exportAllowed_) return proceed();
  auto *panel = new ConfirmPanel(host_, t("security.export.title"), t("security.export.description"), t("security.export.action"), [this, proceed = std::move(proceed)] {
    exportAllowed_ = true;
    proceed();
  }, false, Popover::DialogSurface);
  panel->setObjectName(QStringLiteral("exportConfirm"));
  openCentered(panel);
}
std::optional<QList<QUrl>> MainWindow::exportUrls(const QVector<MediaItem> &items) {
  if (!exportAllowed_) return std::nullopt;
  QList<QUrl> urls;
  QString error;
  for (const MediaItem &item : items) {
    const QString copy = SecureMedia::isSealed(item.path()) ? SecureMedia::exportCopy(item.path(), item.label(), &error) : item.path();
    if (!copy.isEmpty()) urls << QUrl::fromLocalFile(copy);
  }
  if (!error.isEmpty()) showToast(1, error);
  return urls;
}
void MainWindow::applyExportGate() {
  if (inProtectedRoot()) gallery_->setExportGate([this](const QVector<MediaItem> &items) { return exportUrls(items); });
  else gallery_->setExportGate({});
}
void MainWindow::setProcessing(bool processing) {
  // Nothing in the room can be reached while it is being indexed or encrypted: no panels, no shortcuts.
  if (processing) {
    Popover::closeAll(host_);
    disabledActions_.clear();
    for (QAction *action : actions())
      if (action->isEnabled()) {
        action->setEnabled(false);
        disabledActions_ << action;
      }
    // The lock screens and the privacy shield stay on top.
    if (locked_) lockScreen_->raise();
    if (vaultScreen_->isVisible()) vaultScreen_->raise();
    if (shield_->isVisible()) shield_->raise();
    return;
  }
  for (QAction *action : std::exchange(disabledActions_, {}))
    if (action) action->setEnabled(true);
  offerEncryption();
}
