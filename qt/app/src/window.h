#pragma once
class QFileDialog;
#include "duplicates.h"
#include "gallery.h"
#include "overlays.h"
#include "panels.h"
#include "root_watcher.h"
#include "security.h"
#include "sidebar.h"
#include "updater.h"
#include "viewer.h"
#include <QMainWindow>
#include <array>
#include <QSet>
#include <QStackedWidget>
class HashPill;
struct UiState {
  int columns = 7;
  double sidebarFraction = 0.2;  // Default 20% of the width, 16% to 24%.
  QString sortKey = QStringLiteral("name"), sortDir = QStringLiteral("asc"), rootDir, decoyRoot, language = QStringLiteral("en"), layout = QStringLiteral("default");
  bool contentProtected = false, scroll60 = false, autoUpdate = true;
  QStringList hardeningNeverAsk;  // Plain roots for which encryption is not offered.
};
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  MainWindow(const QString &profile, const QString &root, const QString &legacyProfile);
  ~MainWindow() override;
  CoreClient *core() { return &core_; }
  GalleryModel *model() { return &model_; }
  GalleryView *gallery() { return gallery_; }
  DuplicatesView *duplicates() { return duplicates_; }
  Viewer *viewer() { return viewer_; }
  AlbumTree *tree() { return tree_; }
  ThumbPipeline *pipeline() { return &pipeline_; }
  ToastHost *toasts() { return toasts_; }
  Updater *updater() { return updater_; }
  QWidget *overlayHost() { return host_; }
  bool coreReady() const { return coreReady_; }
  bool albumsReady() const { return albumsReady_; }
  int hashDone() const { return hashDone_; }
  int hashTotal() const { return hashTotal_; }
  qint64 processed() const { return processed_; }
  bool openRoot(const QString &root);
  void activateAlbum(const QString &id, std::function<void()> done = {});
  void openViewer(int index);
  void stepViewer(int delta);
  int viewerIndex() const { return viewerIndex_; }
  void showDuplicates(bool show, std::function<void()> done = {});
  bool duplicatesVisible() const { return stack_->currentWidget() == duplicates_; }
  // Mutations return through done(error).
  void moveItems(const QVector<MediaItem> &items, const QString &targetId, std::function<void(QString)> done = {});
  void deleteItems(const QVector<MediaItem> &items, std::function<void(QString)> done = {});
  void importPaths(const QStringList &paths, const QString &albumId, std::function<void(QString)> done = {});
  void refresh(std::function<void()> done = {});
  void moveAlbum(const QString &albumId, const QString &parentId, std::function<void(QString)> done = {});
  void renameAlbum(const QString &albumId, const QString &name, std::function<void(QString)> done = {});
  void deleteAlbum(const QString &albumId, std::function<void(QString)> done = {});
  void createAlbum(const QString &parentPath, const QString &name, std::function<void()> done = {});
  // Scope of the inventory scan after the last createAlbum: "album:<id>" or "library" (QA readback).
  const QString &lastAlbumScan() const { return lastAlbumScan_; }
  void openSettings();
  void openAdvancedSettings();
  void setLayoutKind(const QString &layout);
  void setSort(const QString &key);
  void setSortDirection(const QString &direction);
  void setLocked(bool locked);
  bool locked() const { return locked_; }
  bool shielded() const { return unfocused_; }
  bool displayDecoy() const { return displayDecoy_; }
  bool lockscreenEnabled() const { return lockscreenEnabled_; }
  bool privacyEnabled() const { return privacyEnabled_; }
  bool hardeningEnabled() const { return hardening_; }
  // The open root is encrypted and unlocked: screenshots are blocked and files leave only decrypted, on request.
  bool inProtectedRoot() const { return !vaultRoot_.isEmpty() && !displayDecoy_; }
  VaultUnlock *vaultScreen() { return vaultScreen_; }
  PreloadOverlay *processingScreen() { return preload_; }
  void setFavorite(const MediaItem &item, bool favorite, std::function<void(QString)> done = {});
  void setItemsDate(const QVector<MediaItem> &items, qint64 timestamp, std::function<void(QString)> done = {});
  void copyItems(const QVector<MediaItem> &items);
  void pasteFromClipboard();
  void applyLanguage(const QString &language);
  void resetRoot();
  void setDebuggerVisible(bool visible);
  QString profile() const { return profile_; }
  const UiState &ui() const { return ui_; }
#ifdef ROOM237_QA
  void setUnfocusedForQa(bool unfocused) { setUnfocused(unfocused); }
  void applySettingsForQa(const QJsonObject &settings) { onSettings(settings); }
  void setColumnsForQa(int columns) { columnsSlider_->setValue(columns, true); }
#endif
signals:
  void coreBecameReady();
  void healthy();
protected:
  void resizeEvent(QResizeEvent *event) override;
  void moveEvent(QMoveEvent *event) override;
  void closeEvent(QCloseEvent *event) override;
  void changeEvent(QEvent *event) override;
  bool eventFilter(QObject *object, QEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
private:
#ifdef ROOM237_QA
  friend class QaDriver;
  QFileDialog *qaRootDialog_ = nullptr;
#endif
  bool rootAllowed(const QString &root) const;
  QString rootDialogStart(bool decoy = false) const;
  // qaStart: the folder a QA run opens the native dialog at; ignored outside QA builds.
  void pickRootDirectory(const QString &kind, const QString &qaStart = {});
  void followRenamedAlbum(const QString &from, const QString &to, const QString &error, std::function<void(QString)> done);
  void acceptChosenRoot(const QString &kind, const QString &path, std::function<void(QString)> done = {});
  bool switchDecoy();
  bool clearDecoy();
  bool uiRootsRefused_ = false;
  void buildContent();
  void buildActions();
  SettingsCallbacks settingsCallbacks();
  void onEvent(const QString &name, const QJsonValue &payload);
  void rebuildTree();
  // The open album can be renamed, moved or removed behind the application's back.
  void followActiveAlbum();
  void refreshFavoriteCounts();
  void showMediaMenu(int index, QPoint global);
  void showAlbumMenu(const QString &albumId, QPoint global);
  void updateHeader();
  void updateStatus();
  void updateHashing();
  void onSettings(const QJsonObject &settings);
  void enterGallery();
  void showStartScreens();
  void showMissingRoot(const QString &root);
  void updateDirectoryTexts();
  void updatePrivacySurface();
  QJsonObject vaultStatus(const QString &root) const;
  void showVaultUnlock(const QString &root);
  void onVaultUnlocked(bool decoy);
  void lockVault();
  void offerEncryption();
  void startHardeningSetup();
  void removeEncryption();
  void replaceFactor();
  void onVaultProgress(const QJsonObject &progress);
  void setProcessing(bool processing);
  // Decrypted copies of `items` for another app, after the user agreed in this unlock.
  void confirmExport(std::function<void()> proceed);
  std::optional<QList<QUrl>> exportUrls(const QVector<MediaItem> &items);
  void applyExportGate();
  void setUnfocused(bool unfocused);
  void scheduleReconcile(const QString &directory);
  void reconcileNow();
  void afterMutation(const QStringList &albums, std::function<void()> done);
  void updateWatcher();
  void probeDuplicates();
  void loadUiState();
  void saveUiState();
  void onViewerClosed();
  // The direction of the last step in the viewer: 1 forward, -1 back.
  int viewerStep_ = 1;
  // The window as it looks under the viewer, for the close animation.
  QPixmap grabBehindViewer();
  void selectedOrItem(int index, std::function<void(QVector<MediaItem>)> done);
  bool galleryKeysAllowed() const;
  void showToast(int kind, const QString &text);
  void resolveNeighbors();
  void refreshTexts();
  QString lastAlbumScan_;
  QString effectiveRoot() const { return displayDecoy_ && !ui_.decoyRoot.isEmpty() ? ui_.decoyRoot : ui_.rootDir; }
  CoreClient core_;
  GalleryModel model_;
  ThumbPipeline pipeline_;
  QWidget *host_, *sidebarPanel_, *sidebar_, *header_, *rightPane_, *sidebarHandle_;
  GalleryView *gallery_;
  DuplicatesView *duplicates_;
  Viewer *viewer_;
  AlbumTree *tree_;
  QStackedWidget *stack_;
  Segmented *layoutControl_;
  TextButton *sortButton_, *dupButton_;
  IconButton *directionButton_, *favoritesButton_, *albumMenuButton_, *keyboardButton_, *settingsButton_;
  NumberSlider *columnsSlider_;
  HashPill *hashPill_;
  ToastHost *toasts_;
  StartupScreen *startupScreen_;
  HeroScreen *directoryScreen_, *confirmScreen_, *lockScreen_;
  TextButton *retryRoot_, *chooseRoot_, *confirmOpen_, *pickAnother_;
  QString missingRoot_;  // Library folder that was not found; empty when the start screen asks for a new one.
  QVariantAnimation lockFade_, workspaceFade_, gearSpin_;
  QWidget *debugger_ = nullptr;
  LoggerPanel *logger_ = nullptr;
  quint64 lastKeyStamp_ = 0;
  int lastKey_ = 0;
  PrivacyShield *shield_;
  VaultUnlock *vaultScreen_;
  QTimer workersSave_;
  std::array<int, 3> preloadWorkers_{4, 4, 4};  // Preview, metadata and encryption threads.
  QList<QPointer<QAction>> disabledActions_;
  QString vaultRoot_;  // The open root when it is encrypted and unlocked.
  QSet<QString> offeredRoots_;
  QTimer vaultRefresh_;
  bool hardening_ = false, exportAllowed_ = false;
  PreloadOverlay *preload_;
  QLabel *dragHint_;
  Updater *updater_;
  RootWatcher watcher_;
  QTimer reconcileTimer_, thumbRefresh_, inactivity_, saveTimer_, windowStateTimer_;
  QString profile_, cliRoot_, keyBuffer_;
  QElapsedTimer keyClock_;
  UiState ui_;
  bool coreReady_ = false, albumsReady_ = false, allowOpen_ = false, settingsLoaded_ = false, preloadRunning_ = false;
  bool privacyEnabled_ = false, lockscreenEnabled_ = false, confirmOpenEnabled_ = false, locked_ = false, unfocused_ = false, displayDecoy_ = false;
  bool reconcileRoot_ = false, healthyMarked_ = false, closing_ = false, resizingSidebar_ = false, batchRunning_ = false;
  QSet<QString> reconcileAlbums_;
  QHash<QString, qint64> lastReconcile_;
  QString activeDirectoryId_;
  int activeMissingAfter_ = -1;  // Listing number at the first miss of the open album; -1 when it was found.
  QStringList knownAlbumIds_;
  QElapsedTimer reconcileClock_;
  int hashDone_ = 0, hashTotal_ = 0, viewerIndex_ = -1, updateToast_ = 0;
  qint64 processed_ = 0;
  QHash<QString, int> favoriteCounts_;
  QVector<MediaItem> previewItems_;
  int previewGroup_ = 0;
  QVariantAnimation startFade_;
};
