#pragma once
// Security hardening in the interface: setting up an encrypted root, unlocking it, removing the encryption.
#include "core_client.h"
#include "widgets.h"
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QStackedLayout>

// The second factors this device offers, as the core reports them.
struct FactorChoice {
  QString kind, detail;
  bool available = false, system = false;
};
QVector<FactorChoice> factorChoices(const QJsonObject &factors);
QString factorTitle(const QString &kind);
QString factorUnavailable(const QString &kind, const QString &detail = {});
bool factorNeedsPin(const QString &kind);
void openCentered(Popover *popover);

// A recovery key typed group by group. Groups that are not asked for show their shape and take no input.
class KeyInput : public QWidget {
  Q_OBJECT
public:
  explicit KeyInput(QWidget *parent = nullptr, int groups = 9, int length = 4);
  void setEditable(const QList<int> &groups);  // All groups when empty.
  QString text() const;                        // The groups joined by dashes.
  QString group(int index) const;
  void setGroups(const QStringList &values);   // Values of the editable groups, in order.
  void clear();
  void focusFirst();
signals:
  void submitted();
private:
  void distribute(int from, const QString &typed);
  void focusCell(int index, bool atEnd);
  int length_;
  QList<int> editable_;
  QVector<QLineEdit *> fields_;
  QVector<QLabel *> fixed_;
};

// Password, decoy password, second factors; then the recovery key.
class HardeningSetup : public Popover {
  Q_OBJECT
public:
  HardeningSetup(QWidget *host, CoreClient *core, const QString &root, bool offerDecoy, std::function<void()> started);
#ifdef ROOM237_QA
  void qaSubmit(const QString &password, const QString &repeat, const QString &decoy, const QStringList &kinds);
  QString qaRecoveryKey() const { return pages_->currentWidget() == keyPage_ ? recoveryKey_ : QString(); }
  QString qaError() const { return error_->isVisible() ? error_->text() : QString(); }
  bool qaFactorsLoaded() const { return !factors_.isEmpty(); }
  bool qaKeyBlurred() const { return !keyShown_ && keyText_->graphicsEffect() != nullptr && !keyText_->text().contains(recoveryKey_.left(9)); }
  // Confirms the saved key with `groups` typed into the check (the right ones when empty) and returns its error.
  QString qaDone(const QStringList &groups = {});
  void qaReveal() { showKey(true); }
  void qaShowCheck() { showCheck(); }
#endif
private:
  void submit();
  void showRecoveryKey(const QString &key);
  void showKey(bool shown);
  void showCheck();
  void check();
  void updatePin();
  void fit();
  CoreClient *core_;
  QString root_, recoveryKey_;
  QStackedLayout *pages_;
  QWidget *form_, *keyPage_, *checkPage_;
  QLineEdit *password_, *repeat_, *decoy_ = nullptr, *pin_;
  KeyInput *checkInput_;
  QLabel *pinLabel_, *error_, *keyText_, *touch_, *checkText_, *checkError_;
  QVector<std::pair<FactorChoice, Switch *>> factors_;
  TextButton *encrypt_;
  IconButton *reveal_;
  int groupA_ = 0, groupB_ = 0;
  bool keyShown_ = false, fidoPin_ = false;
  std::function<void()> started_;
};

// Shown once per root and run when hardening is on and the open root is not encrypted.
class EncryptOffer : public Popover {
  Q_OBJECT
public:
  EncryptOffer(QWidget *host, const QString &rootName, std::function<void()> encrypt, std::function<void()> later, std::function<void()> never);
#ifdef ROOM237_QA
  void qaChoose(const QString &choice);
private:
  QHash<QString, TextButton *> choices_;
#endif
};

// Confirms with the password, then decrypts every file of the root in the background.
class RemoveEncryption : public Popover {
  Q_OBJECT
public:
  RemoveEncryption(QWidget *host, CoreClient *core, const QString &root, std::function<void()> started);
};

// A second factor for this device, with the recovery key and the password (after new fingerprints or on another
// computer).
class ReplaceFactor : public Popover {
  Q_OBJECT
public:
  ReplaceFactor(QWidget *host, CoreClient *core, const QString &root, std::function<void(QString)> done);
};

// Full-window unlock: the password and a second factor, the recovery key instead of the factor, or the factor alone
// after a lock during this run.
class VaultUnlock : public QWidget {
  Q_OBJECT
public:
  VaultUnlock(QWidget *parent, CoreClient *core);
  // status: vault_status of the root.
  void start(const QString &root, const QJsonObject &status);
  const QString &root() const { return root_; }
  bool usedRecovery() const { return usedRecovery_; }
  void retranslate();
#ifdef ROOM237_QA
  // Types into the visible fields and presses the primary (or second factor) button.
  void qaSubmit(const QString &password, const QString &recoveryKey, bool useRecovery, bool secondary);
  QJsonObject qaState() const;
#endif
signals:
  // decoy: the decoy password was entered; the real root stays locked.
  void unlocked(bool decoy);
protected:
  void paintEvent(QPaintEvent *) override;
  void showEvent(QShowEvent *) override;
private:
  void run(const QString &kind);
  void setBusy(bool busy);
  void layoutMode();
  CoreClient *core_;
  QString root_;
  QStringList kinds_, pinKinds_;
  bool resumable_ = false, recovery_ = false, busy_ = false, usedRecovery_ = false;
  QLabel *title_, *subtitle_, *error_;
  QLineEdit *password_, *pin_;
  KeyInput *recoveryKey_;
  TextButton *primary_, *secondary_, *useRecovery_, *usePassword_;
};
