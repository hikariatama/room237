#pragma once
// Popovers and dialogs built from the shared controls.
#include "core_client.h"
#include "widgets.h"
#include <QHash>
#include <QJsonObject>
#include <QLabel>
class QVBoxLayout;
class TextPrompt : public Popover {
  Q_OBJECT
public:
  TextPrompt(QWidget *host, const QString &title, const QString &actionText, const QString &initial, std::function<void(QString)> accepted);
  void setBusy(bool busy);
private:
  QLineEdit *field_;
  TextButton *accept_;
};
class ConfirmPanel : public Popover {
  Q_OBJECT
public:
  ConfirmPanel(QWidget *host, const QString &title, const QString &description, const QString &actionText, std::function<void()> accepted, bool dangerous = true, Surface surface = PopoverSurface);
  void setFixedPanelWidth(int width);
  void setActionIcon(const QByteArray &icon);
private:
  QLabel *heading_, *detail_;
  TextButton *accept_;
};
class DeleteSubmenu : public Popover {
  Q_OBJECT
public:
  DeleteSubmenu(QWidget *host, const QString &title, const QString &warning, const QString &action, std::function<void()> accepted);
};
class AlbumActionsPanel : public Popover {
  Q_OBJECT
public:
  AlbumActionsPanel(QWidget *host, const QString &album, const QByteArray &revealIcon, std::function<void()> reveal, std::function<void()> copyPath, std::function<void()> remove);
};
class DatePanel : public Popover {
  Q_OBJECT
public:
  DatePanel(QWidget *host, const QDateTime &value, std::function<void(QDateTime)> accepted);
};
class HotkeyPanel : public Popover {
  Q_OBJECT
public:
  HotkeyPanel(QWidget *host, bool albumSelected, bool hasSelection);
protected:
  void paintEvent(QPaintEvent *event) override;
private:
  bool album_, selection_;
};
struct SettingsCallbacks {
  std::function<void()> changeRoot, pickDecoy, clearDecoy, openAdvanced, checkUpdates;
  std::function<void(bool)> setContentProtected, setAutoUpdate, setScroll60;
  std::function<void(QString)> setLanguage;
  std::function<QString()> language, decoyRoot;
  std::function<bool()> privacyEnabled, contentProtected, autoUpdate, scroll60;
  QString version;
  bool showRootControls = true;
};
class SettingsPanel : public Popover {
  Q_OBJECT
public:
  SettingsPanel(QWidget *host, SettingsCallbacks callbacks);
};
class AdvancedDialog : public Popover {
  Q_OBJECT
public:
  AdvancedDialog(QWidget *host, CoreClient *core, SettingsCallbacks app, std::function<void(QJsonObject)> applied, std::function<void(int kind, QString)> notify);
protected:
  void resizeEvent(QResizeEvent *event) override;
private:
  void buildNavigation();
  void showSection(const QString &key);
  void refreshState();
  QWidget *numberField(const char *section, const char *key, const char *label, const char *helper, int low, int high);
  QWidget *sliderField(const char *section, const char *key, const char *label, const char *helper, int low, int high);
  QWidget *boolField(const char *section, const char *key, const char *label, const char *helper);
  QWidget *enumField(const char *section, const char *key, const char *label, const char *helper, const QVector<std::pair<QString, QString>> &options);
  QWidget *textField(const char *section, const char *key, const char *label, const char *helper);
  QWidget *row(const QString &label, const QString &helper, QWidget *control, const char *section, const char *key);
  CoreClient *core_;
  SettingsCallbacks app_;
  QJsonObject settings_, original_, defaults_;
  QVBoxLayout *sections_, *nav_;
  std::function<void(QJsonObject)> applied_;
  std::function<void(int, QString)> notify_;
  QHash<QString, TextButton *> navButtons_;
  QString active_;
  TextButton *save_ = nullptr;
  IconButton *closeButton_ = nullptr;
};
