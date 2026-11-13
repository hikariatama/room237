#include "security.h"

#include "i18n.h"
#include "overlays.h"
#include "theme.h"

#include <QClipboard>
#include <QGraphicsBlurEffect>
#include <QRandomGenerator>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QLabel *text(const QString &value, int size, QFont::Weight weight, const char *color) {
  auto *out = new QLabel(value);
  out->setFont(Theme::font(size, weight));
  out->setStyleSheet(QStringLiteral("color:%1;background:transparent").arg(QLatin1String(color)));
  out->setWordWrap(true);
  out->setTextFormat(Qt::PlainText);
  return out;
}
QLineEdit *secretField(const QString &placeholder) {
  auto *field = new TextField;
  field->setFont(Theme::font(16));
  field->setEchoMode(QLineEdit::Password);
  field->setPlaceholderText(placeholder);
  return field;
}
QLabel *errorLabel() {
  QLabel *out = text({}, 12, QFont::Normal, "#ff6467");
  out->hide();
  return out;
}
void showError(QLabel *label, const QString &message) {
  label->setText(message);
  label->setVisible(!message.isEmpty());
}
// Messages of the core in the interface's language.
QString explain(const QString &error) {
  if (error.contains(QLatin1String("Wrong password"))) return t("security.error.wrongPassword");
  if (error.contains(QLatin1String("cancelled"))) return t("security.error.cancelled");
  if (error.contains(QLatin1String("fingerprints changed"))) return t("security.error.invalidated");
  if (error.contains(QLatin1String("recovery key is mistyped"))) return t("security.error.recoveryKey");
  if (error.contains(QLatin1String("not set up for this library"))) return t("security.error.factorMissing");
  if (error.contains(QLatin1String("Enter the password"))) return t("security.error.needPassword");
  if (error.contains(QLatin1String("Wrong PIN of the TPM"))) return t("security.error.tpm.pinWrong");
  if (error.contains(QLatin1String("TPM is locked"))) return t("security.error.tpm.lockout");
  if (error.contains(QLatin1String("belongs to another TPM"))) return t("security.error.tpm.otherTpm");
  if (error.contains(QLatin1String("Enter the TPM PIN")) || error.contains(QLatin1String("Choose a PIN for the TPM"))) return t("security.error.tpm.pin");
  if (error.contains(QLatin1String("owner hierarchy has a password"))) return t("security.error.tpm.owner");
  if (error.contains(QLatin1String("Windows Hello key of this root no longer exists"))) return t("security.error.windows.gone");
  if (error.contains(QLatin1String("Windows Hello is locked"))) return t("security.error.windows.locked");
  if (error.contains(QLatin1String("Connect a security key"))) return t("security.error.fido.connect");
  if (error.contains(QLatin1String("needs its PIN"))) return t("security.error.fido.pinRequired");
  if (error.contains(QLatin1String("Wrong PIN"))) return t("security.error.fido.pinWrong");
  if (error.contains(QLatin1String("PIN of the security key is blocked"))) return t("security.error.fido.pinBlocked");
  if (error.contains(QLatin1String("not touched in time"))) return t("security.error.fido.timeout");
  if (error.contains(QLatin1String("does not support hmac-secret"))) return t("security.error.fido.unsupported");
  if (error.contains(QLatin1String("does not hold the key"))) return t("security.error.fido.otherKey");
  return error;
}
// Text of the key's shape for the blurred spoiler: the real key is not on screen until it is revealed.
QString decoyLike(const QString &key) {
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  QString out = key;
  for (QChar &c : out)
    if (c != QLatin1Char('-')) c = QLatin1Char(alphabet[QRandomGenerator::system()->bounded(int(sizeof alphabet - 1))]);
  return out;
}
}  // namespace

QVector<FactorChoice> factorChoices(const QJsonObject &factors) {
  QVector<FactorChoice> out;
  for (const QJsonValue &value : factors.value(QLatin1String("factors")).toArray()) {
    const QJsonObject o = value.toObject();
    out.push_back({o.value(QLatin1String("kind")).toString(), o.value(QLatin1String("detail")).toString(), o.value(QLatin1String("available")).toBool(), o.value(QLatin1String("system")).toBool()});
  }
  return out;
}
QString factorTitle(const QString &kind) {
  if (kind == QLatin1String("macos-se")) return t("security.factor.mac");
  if (kind == QLatin1String("windows-hello")) return t("security.factor.windows");
  if (kind == QLatin1String("tpm2")) return t("security.factor.tpm");
  if (kind == QLatin1String("fido2")) return t("security.factor.fido");
  if (kind == QLatin1String("recovery")) return t("security.recoveryKey");
  return t("security.factor.test");
}
QString factorUnavailable(const QString &kind, const QString &detail) {
  if (kind == QLatin1String("macos-se")) return t("security.factor.macUnavailable");
  if (kind == QLatin1String("fido2")) return t("security.factor.fidoUnavailable");
  if (detail.contains(QLatin1String("tpm2-tss"))) return t("security.factor.tpmMissing");
  if (detail.contains(QLatin1String("No TPM is available"))) return t("security.factor.tpmAccess");
  if (detail.contains(QLatin1String("Windows Hello is not set up"))) return t("security.factor.windowsSetup");
  return t("security.factor.unavailable");
}
// The TPM always takes a PIN; a security key only when it has one, which it reports when asked without it.
bool factorNeedsPin(const QString &kind) { return kind == QLatin1String("tpm2"); }
void openCentered(Popover *popover) {
  QWidget *host = popover->parentWidget();
  const QSize size = popover->panelRect().size();
  popover->setModalDim(true);
  popover->openAt(QRect(host->width() / 2, (host->height() - size.height()) / 2 - 7, 1, 1), Qt::BottomEdge, Qt::AlignHCenter);
}

// --- Setup ---------------------------------------------------------------------------------------------------------

HardeningSetup::HardeningSetup(QWidget *host, CoreClient *core, const QString &root, bool offerDecoy, std::function<void()> started)
    : Popover(host, DialogSurface), core_(core), root_(root), started_(std::move(started)) {
  setDismissOnOutside(false);
  pages_ = new QStackedLayout(content_);
  form_ = new QWidget;
  auto *layout = new QVBoxLayout(form_);
  layout->setContentsMargins(20, 20, 20, 20);
  layout->setSpacing(10);
  layout->addWidget(text(t("security.setup.title", {{"folder", QFileInfo(root).fileName()}}), 16, QFont::DemiBold, "#ffffff"));
  layout->addWidget(text(t("security.setup.description"), 12, QFont::Normal, "#8a8a8a"));
  layout->addSpacing(4);
  password_ = secretField(t("security.password"));
  repeat_ = secretField(t("security.passwordRepeat"));
  layout->addWidget(password_);
  layout->addWidget(repeat_);
  layout->addWidget(text(t("security.passwordHint"), 12, QFont::Normal, "#636363"));
  if (offerDecoy) {
    layout->addSpacing(6);
    layout->addWidget(text(t("security.decoyPassword"), 12, QFont::Medium, "#8a8a8a"));
    decoy_ = secretField(t("security.decoyPasswordPlaceholder"));
    layout->addWidget(decoy_);
    layout->addWidget(text(t("security.decoyPasswordHint"), 12, QFont::Normal, "#636363"));
  }
  layout->addSpacing(6);
  layout->addWidget(text(t("security.secondFactor"), 12, QFont::Medium, "#8a8a8a"));
  pin_ = secretField(t("security.pin"));
  pinLabel_ = text({}, 12, QFont::Normal, "#636363");
  auto *factorRows = new QVBoxLayout;
  factorRows->setSpacing(8);
  layout->addLayout(factorRows);
  layout->addWidget(pin_);
  layout->addWidget(pinLabel_);
  // The dialog can close before the device answers which factors it has.
  QPointer<HardeningSetup> guard(this);
  core_->call(QStringLiteral("vault_factors"), {}, [this, guard, factorRows](const QJsonValue &value, const QString &) {
    if (!guard) return;
    bool first = true;
    for (const FactorChoice &choice : factorChoices(value.toObject())) {
      auto *row = new QHBoxLayout;
      row->setSpacing(12);
      auto *texts = new QVBoxLayout;
      texts->setSpacing(0);
      texts->addWidget(text(factorTitle(choice.kind), 14, QFont::Normal, choice.available ? "#ffffff" : "#636363"));
      if (!choice.available) texts->addWidget(text(factorUnavailable(choice.kind, choice.detail), 12, QFont::Normal, "#636363"));
      row->addLayout(texts, 1);
      auto *toggle = new Switch(factorTitle(choice.kind));
      toggle->setEnabled(choice.available);
      // The system prompt is the default; a security key is added on request.
      toggle->setChecked(choice.available && choice.system && first);
      if (choice.available && choice.system) first = false;
      row->addWidget(toggle, 0, Qt::AlignVCenter);
      factorRows->addLayout(row);
      factors_.push_back({choice, toggle});
      connect(toggle, &QAbstractButton::toggled, this, &HardeningSetup::updatePin);
    }
    updatePin();
  });
  error_ = errorLabel();
  layout->addWidget(error_);
  touch_ = text(t("security.touchKey"), 12, QFont::Medium, "#ffffff");
  touch_->hide();
  layout->addWidget(touch_);
  auto *buttons = new QHBoxLayout;
  buttons->setSpacing(8);
  buttons->addStretch();
  auto *cancel = new TextButton(t("common.cancel"), TextButton::Outline);
  encrypt_ = new TextButton(t("security.encrypt"), TextButton::Primary);
  encrypt_->setIcon("lock");
  for (TextButton *button : {cancel, encrypt_}) button->setSmall(true);
  buttons->addWidget(cancel);
  buttons->addWidget(encrypt_);
  layout->addSpacing(4);
  layout->addLayout(buttons);
  connect(cancel, &QAbstractButton::clicked, this, &Popover::animateClose);
  connect(encrypt_, &QAbstractButton::clicked, this, &HardeningSetup::submit);
  for (QLineEdit *field : {password_, repeat_}) connect(field, &QLineEdit::returnPressed, this, &HardeningSetup::submit);

  keyPage_ = new QWidget;
  auto *keys = new QVBoxLayout(keyPage_);
  keys->setContentsMargins(20, 20, 20, 20);
  keys->setSpacing(10);
  keys->addWidget(text(t("security.recovery.title"), 16, QFont::DemiBold, "#ffffff"));
  keys->addWidget(text(t("security.recovery.description"), 12, QFont::Normal, "#8a8a8a"));
  auto *warning = new QHBoxLayout;
  warning->setSpacing(8);
  auto *icon = new QLabel;
  icon->setPixmap(Theme::icon("alert-triangle", 16, QColor(0xff, 0xb9, 0x00), devicePixelRatioF()));
  icon->setStyleSheet(QStringLiteral("background:transparent"));
  warning->addWidget(icon, 0, Qt::AlignTop);
  warning->addWidget(text(t("security.recovery.warning"), 12, QFont::Normal, "#ffb900"), 1);
  keys->addLayout(warning);
  // The key itself, large; hidden, it is a blurred text of random characters.
  keyText_ = new QLabel;
  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono.setPixelSize(22);
  mono.setWeight(QFont::Medium);
  keyText_->setFont(mono);
  keyText_->setStyleSheet(QStringLiteral("color:#ffffff;background:transparent"));
  keyText_->setAlignment(Qt::AlignCenter);
  keyText_->setTextInteractionFlags(Qt::NoTextInteraction);
  keyText_->setContentsMargins(0, 14, 0, 6);
  keys->addWidget(keyText_);
  auto *keyButtons = new QHBoxLayout;
  keyButtons->setSpacing(8);
  keyButtons->addStretch();
  reveal_ = new IconButton("eye", t("security.recovery.show"), 36);
  auto *copy = new IconButton("copy", t("security.recovery.copy"), 36);
  keyButtons->addWidget(reveal_);
  keyButtons->addWidget(copy);
  keyButtons->addStretch();
  keys->addLayout(keyButtons);
  auto *done = new TextButton(t("security.recovery.done"), TextButton::Primary);
  done->setSmall(true);
  auto *doneRow = new QHBoxLayout;
  doneRow->addStretch();
  doneRow->addWidget(done);
  keys->addSpacing(4);
  keys->addLayout(doneRow);
  connect(reveal_, &QAbstractButton::clicked, this, [this] { showKey(!keyShown_); });
  connect(copy, &QAbstractButton::clicked, this, [this] {
    QGuiApplication::clipboard()->setText(recoveryKey_);
    // The key leaves the clipboard after a minute unless something else replaced it.
    const QString copied = recoveryKey_;
    QTimer::singleShot(60000, qApp, [copied] {
      if (QGuiApplication::clipboard()->text() == copied) QGuiApplication::clipboard()->clear();
    });
  });
  connect(done, &QAbstractButton::clicked, this, &HardeningSetup::showCheck);

  // Two groups of the key, typed back, show that it was written down.
  checkPage_ = new QWidget;
  auto *checks = new QVBoxLayout(checkPage_);
  checks->setContentsMargins(20, 20, 20, 20);
  checks->setSpacing(10);
  checks->addWidget(text(t("security.recovery.check.title"), 16, QFont::DemiBold, "#ffffff"));
  checkText_ = text({}, 12, QFont::Normal, "#8a8a8a");
  checks->addWidget(checkText_);
  checks->addSpacing(6);
  checkInput_ = new KeyInput;
  checks->addWidget(checkInput_, 0, Qt::AlignHCenter);
  checkError_ = errorLabel();
  checks->addWidget(checkError_);
  auto *checkButtons = new QHBoxLayout;
  checkButtons->setSpacing(8);
  auto *back = new TextButton(t("security.recovery.check.back"), TextButton::Ghost);
  auto *confirm = new TextButton(t("security.recovery.check.action"), TextButton::Primary);
  for (TextButton *button : {back, confirm}) button->setSmall(true);
  checkButtons->addWidget(back);
  checkButtons->addStretch();
  checkButtons->addWidget(confirm);
  checks->addSpacing(6);
  checks->addLayout(checkButtons);
  connect(back, &QAbstractButton::clicked, this, [this] {
    pages_->setCurrentWidget(keyPage_);
    fit();
  });
  connect(confirm, &QAbstractButton::clicked, this, &HardeningSetup::check);
  connect(checkInput_, &KeyInput::submitted, this, &HardeningSetup::check);
  pages_->addWidget(form_);
  pages_->addWidget(keyPage_);
  pages_->addWidget(checkPage_);
  connect(this, &Popover::closed, this, &QObject::deleteLater);
  fit();
  QTimer::singleShot(0, password_, qOverload<>(&QWidget::setFocus));
}
void HardeningSetup::fit() {
  // The key and its check need the room of a whole key on one line.
  QWidget *page = pages_->currentWidget();
  const int width = page == form_ ? 400 : 660;
  setPanelSize({width, page->layout()->totalHeightForWidth(width) + 2});
}
void HardeningSetup::updatePin() {
  bool tpm = false, fido = false;
  for (const auto &[choice, toggle] : factors_) {
    if (!toggle->isChecked()) continue;
    tpm |= choice.kind == QLatin1String("tpm2");
    fido |= choice.kind == QLatin1String("fido2") && fidoPin_;
  }
  QStringList hints;
  if (tpm) hints << t("security.pinHint.tpm");
  if (fido) hints << t("security.pinHint.fido");
  pinLabel_->setText(hints.join(QLatin1Char(' ')));
  pin_->setVisible(tpm || fido);
  pinLabel_->setVisible(tpm || fido);
  fit();
}
void HardeningSetup::showKey(bool shown) {
  keyShown_ = shown;
  keyText_->setText(shown ? recoveryKey_ : decoyLike(recoveryKey_));
  if (shown) {
    keyText_->setGraphicsEffect(nullptr);
  } else {
    auto *blur = new QGraphicsBlurEffect(keyText_);
    blur->setBlurRadius(40);
    blur->setBlurHints(QGraphicsBlurEffect::QualityHint);
    keyText_->setGraphicsEffect(blur);
  }
  keyText_->setAccessibleName(shown ? recoveryKey_ : t("security.recovery.hidden"));
  reveal_->setIcon(shown ? "eye-off" : "eye");
  reveal_->setLabel(shown ? t("security.recovery.hide") : t("security.recovery.show"));
}
void HardeningSetup::showCheck() {
  const qsizetype groups = recoveryKey_.split(QLatin1Char('-')).size();
  groupA_ = QRandomGenerator::system()->bounded(int(groups));
  do groupB_ = QRandomGenerator::system()->bounded(int(groups));
  while (groupB_ == groupA_);
  if (groupB_ < groupA_) std::swap(groupA_, groupB_);
  checkText_->setText(t("security.recovery.check.description", {{"a", QString::number(groupA_ + 1)}, {"b", QString::number(groupB_ + 1)}}));
  checkInput_->setEditable({groupA_, groupB_});
  checkInput_->clear();
  showError(checkError_, {});
  showKey(false);
  pages_->setCurrentWidget(checkPage_);
  fit();
  QTimer::singleShot(0, checkInput_, &KeyInput::focusFirst);
}
void HardeningSetup::check() {
  const QStringList groups = recoveryKey_.split(QLatin1Char('-'));
  if (checkInput_->group(groupA_) != groups.value(groupA_) || checkInput_->group(groupB_) != groups.value(groupB_)) {
    showError(checkError_, t("security.recovery.check.mismatch"));
    fit();
    return;
  }
  recoveryKey_.fill(QLatin1Char('0'));
  animateClose();
}
void HardeningSetup::submit() {
  if (encrypt_->isHidden() || !encrypt_->isEnabled()) return;
  const QString password = password_->text();
  QStringList kinds;
  for (const auto &[choice, toggle] : factors_)
    if (toggle->isChecked()) kinds << choice.kind;
  QString problem;
  if (password.size() < 8) problem = t("security.error.short");
  else if (password != repeat_->text()) problem = t("security.error.mismatch");
  else if (decoy_ && !decoy_->text().isEmpty() && decoy_->text() == password) problem = t("security.error.decoySame");
  else if (kinds.isEmpty()) problem = t("security.error.noFactor");
  else if (kinds.contains(QStringLiteral("tpm2")) && pin_->text().isEmpty()) problem = t("security.error.pin");
  showError(error_, problem);
  fit();
  if (!problem.isEmpty()) return;
  encrypt_->setBusy(true);
  encrypt_->setEnabled(false);
  touch_->setVisible(kinds.contains(QStringLiteral("fido2")));
  fit();
  const QJsonObject params{{QStringLiteral("root"), root_}, {QStringLiteral("password"), password}, {QStringLiteral("factors"), QJsonArray::fromStringList(kinds)}, {QStringLiteral("decoyPassword"), decoy_ ? decoy_->text() : QString()}, {QStringLiteral("reason"), t("security.prompt.setup")}, {QStringLiteral("pin"), pin_->text()}};
  QPointer<HardeningSetup> self(this);
  core_->call(QStringLiteral("vault_setup"), params, [self](const QJsonValue &value, const QString &error) {
    if (!self) return;
    self->encrypt_->setBusy(false);
    self->encrypt_->setEnabled(true);
    self->touch_->hide();
    if (!error.isEmpty()) {
      // A security key with a PIN asks for it; the field appears now.
      if (error.contains(QLatin1String("needs its PIN")) && !self->fidoPin_) {
        self->fidoPin_ = true;
        self->updatePin();
        self->pin_->setFocus();
      }
      showError(self->error_, explain(error));
      self->fit();
      return;
    }
    for (QLineEdit *field : {self->password_, self->repeat_, self->decoy_, self->pin_})
      if (field) field->clear();
    self->showRecoveryKey(value.toObject().value(QLatin1String("recoveryKey")).toString());
    if (self->started_) self->started_();
  });
}
void HardeningSetup::showRecoveryKey(const QString &key) {
  recoveryKey_ = key;
  showKey(false);
  pages_->setCurrentWidget(keyPage_);
  fit();
}

// --- Offer for a plain root ----------------------------------------------------------------------------------------

EncryptOffer::EncryptOffer(QWidget *host, const QString &rootName, std::function<void()> encrypt, std::function<void()> later, std::function<void()> never) : Popover(host, DialogSurface) {
  setDismissOnOutside(false);
  auto *layout = new QVBoxLayout(content_);
  layout->setContentsMargins(20, 20, 20, 20);
  layout->setSpacing(10);
  layout->addWidget(text(t("security.offer.title", {{"folder", rootName}}), 16, QFont::DemiBold, "#ffffff"));
  layout->addWidget(text(t("security.offer.description"), 12, QFont::Normal, "#8a8a8a"));
  auto *buttons = new QHBoxLayout;
  buttons->setSpacing(8);
  auto *neverButton = new TextButton(t("security.offer.never"), TextButton::Ghost);
  auto *laterButton = new TextButton(t("security.offer.later"), TextButton::Outline);
  auto *encryptButton = new TextButton(t("security.encrypt"), TextButton::Primary);
  for (TextButton *button : {neverButton, laterButton, encryptButton}) button->setSmall(true);
  buttons->addWidget(neverButton);
  buttons->addStretch();
  buttons->addWidget(laterButton);
  buttons->addWidget(encryptButton);
  layout->addSpacing(4);
  layout->addLayout(buttons);
  const auto choose = [this](std::function<void()> action) {
    return [this, action] {
      closeNow();
      if (action) action();
    };
  };
#ifdef ROOM237_QA
  choices_ = {{QStringLiteral("never"), neverButton}, {QStringLiteral("later"), laterButton}, {QStringLiteral("encrypt"), encryptButton}};
#endif
  connect(neverButton, &QAbstractButton::clicked, this, choose(std::move(never)));
  connect(laterButton, &QAbstractButton::clicked, this, choose(std::move(later)));
  connect(encryptButton, &QAbstractButton::clicked, this, choose(std::move(encrypt)));
  connect(this, &Popover::closed, this, &QObject::deleteLater);
  setPanelSize({420, layout->totalHeightForWidth(420) + 2});
}

// --- Removal -------------------------------------------------------------------------------------------------------

RemoveEncryption::RemoveEncryption(QWidget *host, CoreClient *core, const QString &root, std::function<void()> started) : Popover(host, DialogSurface) {
  auto *layout = new QVBoxLayout(content_);
  layout->setContentsMargins(20, 20, 20, 20);
  layout->setSpacing(10);
  layout->addWidget(text(t("security.remove.title", {{"folder", QFileInfo(root).fileName()}}), 16, QFont::DemiBold, "#ffffff"));
  layout->addWidget(text(t("security.remove.description"), 12, QFont::Normal, "#8a8a8a"));
  QLineEdit *password = secretField(t("security.password"));
  layout->addWidget(password);
  QLabel *error = errorLabel();
  layout->addWidget(error);
  auto *buttons = new QHBoxLayout;
  buttons->setSpacing(8);
  buttons->addStretch();
  auto *cancel = new TextButton(t("common.cancel"), TextButton::Outline);
  auto *remove = new TextButton(t("security.remove.action"), TextButton::Danger);
  for (TextButton *button : {cancel, remove}) button->setSmall(true);
  buttons->addWidget(cancel);
  buttons->addWidget(remove);
  layout->addSpacing(4);
  layout->addLayout(buttons);
  const auto resize = [this, layout] { setPanelSize({380, layout->totalHeightForWidth(380) + 2}); };
  const auto submit = [this, core, root, password, error, remove, resize, started = std::move(started)] {
    if (password->text().isEmpty() || !remove->isEnabled()) return;
    remove->setBusy(true);
    remove->setEnabled(false);
    QPointer<RemoveEncryption> self(this);
    core->call(QStringLiteral("vault_verify_password"), {{QStringLiteral("root"), root}, {QStringLiteral("password"), password->text()}}, [self, core, root, password, error, remove, resize, started](const QJsonValue &value, const QString &failure) {
      if (!self) return;
      if (!failure.isEmpty() || !value.toBool()) {
        remove->setBusy(false);
        remove->setEnabled(true);
        showError(error, failure.isEmpty() ? t("security.error.wrongPassword") : explain(failure));
        resize();
        return;
      }
      password->clear();
      core->call(QStringLiteral("vault_decrypt"), {{QStringLiteral("root"), root}}, [self, error, remove, resize, started](const QJsonValue &, const QString &failure) {
        if (!self) return;
        remove->setBusy(false);
        remove->setEnabled(true);
        if (!failure.isEmpty()) {
          showError(error, explain(failure));
          resize();
          return;
        }
        self->closeNow();
        if (started) started();
      });
    });
  };
  connect(cancel, &QAbstractButton::clicked, this, &Popover::animateClose);
  connect(remove, &QAbstractButton::clicked, this, submit);
  connect(password, &QLineEdit::returnPressed, this, submit);
  connect(this, &Popover::closed, this, &QObject::deleteLater);
  resize();
  QTimer::singleShot(0, password, qOverload<>(&QWidget::setFocus));
}

// --- New factor ----------------------------------------------------------------------------------------------------

ReplaceFactor::ReplaceFactor(QWidget *host, CoreClient *core, const QString &root, std::function<void(QString)> done) : Popover(host, DialogSurface) {
  auto *layout = new QVBoxLayout(content_);
  layout->setContentsMargins(20, 20, 20, 20);
  layout->setSpacing(10);
  layout->addWidget(text(t("security.replace.title"), 16, QFont::DemiBold, "#ffffff"));
  layout->addWidget(text(t("security.replace.description"), 12, QFont::Normal, "#8a8a8a"));
  auto *kinds = new QVBoxLayout;
  kinds->setSpacing(6);
  layout->addLayout(kinds);
  QLineEdit *pin = secretField(t("security.pin"));
  pin->hide();
  layout->addWidget(pin);
  QLabel *error = errorLabel();
  layout->addWidget(error);
  auto *cancel = new TextButton(t("common.cancel"), TextButton::Outline);
  cancel->setSmall(true);
  auto *buttons = new QHBoxLayout;
  buttons->addStretch();
  buttons->addWidget(cancel);
  layout->addSpacing(4);
  layout->addLayout(buttons);
  const auto resize = [this, layout] { setPanelSize({380, layout->totalHeightForWidth(380) + 2}); };
  connect(cancel, &QAbstractButton::clicked, this, &Popover::animateClose);
  connect(this, &Popover::closed, this, &QObject::deleteLater);
  QPointer<ReplaceFactor> guard(this);
  core->call(QStringLiteral("vault_factors"), {}, [this, guard, core, root, kinds, pin, error, resize, done](const QJsonValue &value, const QString &) {
    if (!guard) return;
    for (const FactorChoice &choice : factorChoices(value.toObject())) {
      if (!choice.available) continue;
      auto *button = new TextButton(factorTitle(choice.kind), TextButton::Outline);
      button->setIcon(choice.system ? "fingerprint" : "key");
      button->setAlignLeft(true);
      kinds->addWidget(button);
      if (factorNeedsPin(choice.kind)) pin->show();
      connect(button, &QAbstractButton::clicked, this, [this, core, root, kind = choice.kind, pin, error, resize, button, done] {
        button->setBusy(true);
        QPointer<ReplaceFactor> self(this);
        core->call(QStringLiteral("vault_add_factor"), {{QStringLiteral("root"), root}, {QStringLiteral("kind"), kind}, {QStringLiteral("reason"), t("security.prompt.setup")}, {QStringLiteral("pin"), pin->text()}}, [self, error, resize, button, done](const QJsonValue &, const QString &failure) {
          if (!self) return;
          button->setBusy(false);
          if (!failure.isEmpty()) {
            if (failure.contains(QLatin1String("needs its PIN"))) self->findChild<QLineEdit *>()->show();
            showError(error, explain(failure));
            resize();
            return;
          }
          self->closeNow();
          if (done) done({});
        });
      });
    }
    resize();
  });
  resize();
}

// --- Unlock screen -------------------------------------------------------------------------------------------------

VaultUnlock::VaultUnlock(QWidget *parent, CoreClient *core) : QWidget(parent), core_(core) {
  auto *column = new QVBoxLayout(this);
  column->setAlignment(Qt::AlignCenter);
  column->setSpacing(0);
  column->addWidget(new LottieView(QStringLiteral("lockscreen"), 104, this), 0, Qt::AlignHCenter);
  title_ = text({}, 20, QFont::DemiBold, "#ffffff");
  title_->setAlignment(Qt::AlignCenter);
  subtitle_ = text({}, 14, QFont::Normal, "#636363");
  subtitle_->setAlignment(Qt::AlignCenter);
  for (QLabel *label : {title_, subtitle_}) label->setFixedWidth(352);
  column->addSpacing(8);
  column->addWidget(title_, 0, Qt::AlignHCenter);
  column->addSpacing(8);
  column->addWidget(subtitle_, 0, Qt::AlignHCenter);
  column->addSpacing(20);
  // Fields and buttons in one column with even gaps; hidden ones leave none.
  auto *form = new QVBoxLayout;
  form->setSpacing(8);
  password_ = secretField(t("security.password"));
  recoveryKey_ = new KeyInput;
  pin_ = secretField(t("security.pin"));
  primary_ = new TextButton({}, TextButton::Primary);
  secondary_ = new TextButton({}, TextButton::Outline);
  error_ = errorLabel();
  error_->setAlignment(Qt::AlignCenter);
  error_->setFixedWidth(320);
  for (QWidget *widget : std::initializer_list<QWidget *>{password_, recoveryKey_, pin_, primary_, secondary_, error_}) form->addWidget(widget, 0, Qt::AlignHCenter);
  column->addLayout(form);
  column->addSpacing(8);
  useRecovery_ = new TextButton(t("security.unlock.useRecovery"), TextButton::Ghost);
  usePassword_ = new TextButton(t("security.unlock.usePassword"), TextButton::Ghost);
  for (TextButton *button : {useRecovery_, usePassword_}) {
    button->setSmall(true);
    column->addWidget(button, 0, Qt::AlignHCenter);
  }
  connect(primary_, &QAbstractButton::clicked, this, [this] { run(recovery_ ? QStringLiteral("recovery") : kinds_.value(0)); });
  connect(secondary_, &QAbstractButton::clicked, this, [this] { run(kinds_.value(1)); });
  connect(useRecovery_, &QAbstractButton::clicked, this, [this] {
    recovery_ = !recovery_;
    if (recovery_) resumable_ = false;
    layoutMode();
  });
  connect(usePassword_, &QAbstractButton::clicked, this, [this] {
    resumable_ = false;
    layoutMode();
  });
  for (QLineEdit *field : {password_, pin_})
    connect(field, &QLineEdit::returnPressed, this, [this] { primary_->click(); });
  connect(recoveryKey_, &KeyInput::submitted, this, [this] { primary_->click(); });
  hide();
}
void VaultUnlock::start(const QString &root, const QJsonObject &status) {
  root_ = root;
  kinds_.clear();
  for (const QJsonValue &kind : status.value(QLatin1String("factors")).toArray()) kinds_ << kind.toString();
  pinKinds_.clear();
  for (const QJsonValue &kind : status.value(QLatin1String("pinFactors")).toArray()) pinKinds_ << kind.toString();
  // The system prompt goes first.
  std::stable_sort(kinds_.begin(), kinds_.end(), [](const QString &a, const QString &b) { return a != QLatin1String("fido2") && b == QLatin1String("fido2"); });
  resumable_ = status.value(QLatin1String("resumable")).toBool();
  recovery_ = false;
  busy_ = false;
  showError(error_, {});
  password_->clear();
  recoveryKey_->clear();
  pin_->clear();
  layoutMode();
}
void VaultUnlock::retranslate() {
  password_->setPlaceholderText(t("security.password"));
  pin_->setPlaceholderText(t("security.pin"));
  useRecovery_->setText(t("security.unlock.useRecovery"));
  usePassword_->setText(t("security.unlock.usePassword"));
  layoutMode();
}
void VaultUnlock::layoutMode() {
  title_->setText(t("security.unlock.title"));
  subtitle_->setText(recovery_ ? t("security.unlock.recoverySubtitle") : resumable_ ? t("security.unlock.resumeSubtitle") : t("security.unlock.subtitle"));
  password_->setVisible(!resumable_);
  recoveryKey_->setVisible(recovery_);
  const QString first = kinds_.value(0);
  pin_->setVisible(!recovery_ && !pinKinds_.isEmpty());
  primary_->setText(recovery_ ? t("security.unlock.withRecovery") : t("security.unlock.with", {{"factor", factorTitle(first)}}));
  primary_->setIcon(recovery_ ? "key" : first == QLatin1String("fido2") ? "key" : "fingerprint");
  secondary_->setVisible(!recovery_ && kinds_.size() > 1);
  if (kinds_.size() > 1) {
    secondary_->setText(t("security.unlock.with", {{"factor", factorTitle(kinds_[1])}}));
    secondary_->setIcon(kinds_[1] == QLatin1String("fido2") ? "key" : "fingerprint");
  }
  useRecovery_->setText(recovery_ ? t("security.unlock.useFactor") : t("security.unlock.useRecovery"));
  usePassword_->setVisible(resumable_ && !recovery_);
  // The fields are as wide as the button under them, which fits the factor's name.
  int width = qMax(288, primary_->sizeHint().width());
  if (!secondary_->isHidden()) width = qMax(width, secondary_->sizeHint().width());
  for (QWidget *widget : std::initializer_list<QWidget *>{password_, pin_, primary_, secondary_}) widget->setFixedWidth(width);
  for (QLabel *label : {title_, subtitle_}) label->setFixedHeight(label->heightForWidth(label->width()));
  if (isVisible()) {
    if (recovery_) recoveryKey_->focusFirst();
    else if (!resumable_) password_->setFocus();
  }
}
void VaultUnlock::setBusy(bool busy) {
  busy_ = busy;
  for (TextButton *button : {primary_, secondary_, useRecovery_, usePassword_}) button->setEnabled(!busy);
}
void VaultUnlock::run(const QString &kind) {
  if (busy_ || kind.isEmpty()) return;
  if (!resumable_ && password_->text().isEmpty()) {
    showError(error_, t("security.error.needPassword"));
    password_->setFocus();
    return;
  }
  if (recovery_ && recoveryKey_->text().remove(QLatin1Char('-')).isEmpty()) {
    recoveryKey_->focusFirst();
    return;
  }
  showError(error_, {});
  setBusy(true);
  if (kind == QLatin1String("fido2")) subtitle_->setText(t("security.touchKey"));
  TextButton *button = kind == kinds_.value(1) && !recovery_ ? secondary_ : primary_;
  button->setBusy(true);
  QPointer<VaultUnlock> self(this);
  usedRecovery_ = recovery_;
  const auto finish = [self, button](const QJsonValue &value, const QString &error) {
    if (!self) return;
    button->setBusy(false);
    self->setBusy(false);
    if (!error.isEmpty()) {
      self->layoutMode();
      showError(self->error_, explain(error));
      return;
    }
    self->password_->clear();
    self->recoveryKey_->clear();
    self->pin_->clear();
    emit self->unlocked(value.toObject().value(QLatin1String("decoy")).toBool());
  };
  if (resumable_ && !recovery_) {
    core_->call(QStringLiteral("vault_resume"), {{QStringLiteral("root"), root_}, {QStringLiteral("kind"), kind}, {QStringLiteral("reason"), t("security.prompt.unlock")}, {QStringLiteral("pin"), pin_->text()}}, finish);
    return;
  }
  core_->call(QStringLiteral("vault_unlock"), {{QStringLiteral("root"), root_}, {QStringLiteral("password"), password_->text()}, {QStringLiteral("kind"), kind}, {QStringLiteral("reason"), t("security.prompt.unlock")}, {QStringLiteral("pin"), pin_->text()}, {QStringLiteral("recoveryKey"), recoveryKey_->text()}}, finish);
}
void VaultUnlock::paintEvent(QPaintEvent *) {
  QPainter p(this);
  Theme::paintPage(p, rect(), mapTo(window(), QPoint(0, 0)));
}
void VaultUnlock::showEvent(QShowEvent *) {
  if (recovery_) QTimer::singleShot(0, recoveryKey_, &KeyInput::focusFirst);
  else if (!resumable_) QTimer::singleShot(0, password_, qOverload<>(&QWidget::setFocus));
}

// --- Recovery key input --------------------------------------------------------------------------------------------

namespace {
// A group cell: Backspace in an empty cell goes back to the previous one.
class GroupField : public TextField {
public:
  std::function<void()> backAtStart;
protected:
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Backspace && text().isEmpty() && backAtStart) {
      backAtStart();
      return;
    }
    TextField::keyPressEvent(event);
  }
};
// Characters of the key's alphabet (base32), upper case.
QString keyCharacters(const QString &typed) {
  QString out;
  for (QChar c : typed) {
    const QChar u = c.toUpper();
    if ((u >= QLatin1Char('A') && u <= QLatin1Char('Z')) || (u >= QLatin1Char('2') && u <= QLatin1Char('7'))) out += u;
  }
  return out;
}
}  // namespace

KeyInput::KeyInput(QWidget *parent, int groups, int length) : QWidget(parent), length_(length) {
  auto *row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(3);
  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono.setPixelSize(16);
  for (int i = 0; i < groups; ++i) {
    if (i > 0) {
      QLabel *dash = ::text(QStringLiteral("-"), 16, QFont::Normal, "#636363");
      dash->setWordWrap(false);
      row->addWidget(dash);
    }
    auto *field = new GroupField;
    field->setMetrics(40, 16);
    field->setStyleSheet(field->styleSheet().replace(QLatin1String("padding:0 13px;"), QLatin1String("padding:0 4px;")).replace(QLatin1String("font-size:16px;"), QStringLiteral("font-size:16px;font-family:\"%1\";").arg(mono.family())));
    field->setFixedWidth(56);
    field->setRadius(10);
    field->setAlignment(Qt::AlignCenter);
    field->setPlaceholderText(QString(length, QLatin1Char('X')));
    field->setAccessibleName(t("security.recovery.check.group", {{"n", QString::number(i + 1)}}));
    QLabel *fixed = ::text(QString(length, QLatin1Char('X')), 16, QFont::Normal, "#4f4f4f");
    fixed->setFont(mono);
    fixed->setStyleSheet(QStringLiteral("color:#4f4f4f;background:transparent;font-family:\"%1\";font-size:16px").arg(mono.family()));
    fixed->setAlignment(Qt::AlignCenter);
    fixed->setFixedWidth(56);
    fixed->setWordWrap(false);
    fixed->hide();
    row->addWidget(field);
    row->addWidget(fixed);
    fields_ << field;
    fixed_ << fixed;
    field->backAtStart = [this, i] {
      for (int j = i - 1; j >= 0; --j)
        if (editable_.contains(j)) return focusCell(j, true);
    };
    connect(field, &QLineEdit::textEdited, this, [this, i](const QString &typed) { distribute(i, typed); });
    connect(field, &QLineEdit::returnPressed, this, &KeyInput::submitted);
  }
  setEditable({});
}
void KeyInput::setEditable(const QList<int> &groups) {
  editable_.clear();
  for (int i = 0; i < fields_.size(); ++i)
    if (groups.isEmpty() || groups.contains(i)) editable_ << i;
  for (int i = 0; i < fields_.size(); ++i) {
    const bool on = editable_.contains(i);
    fields_[i]->setVisible(on);
    fixed_[i]->setVisible(!on);
    // Asked groups among fixed ones are empty slots; a whole key to type shows its shape.
    fields_[i]->setPlaceholderText(groups.isEmpty() ? QString(length_, QLatin1Char('X')) : QString());
  }
}
QString KeyInput::text() const {
  QStringList out;
  for (QLineEdit *field : fields_) out << field->text();
  return out.join(QLatin1Char('-'));
}
QString KeyInput::group(int index) const { return index >= 0 && index < fields_.size() ? fields_[index]->text() : QString(); }
void KeyInput::setGroups(const QStringList &values) {
  for (int k = 0; k < editable_.size(); ++k) fields_[editable_[k]]->setText(keyCharacters(values.value(k)).left(length_));
}
void KeyInput::clear() {
  for (QLineEdit *field : fields_) field->clear();
}
void KeyInput::focusFirst() {
  if (!editable_.isEmpty()) focusCell(editable_.first(), false);
}
void KeyInput::focusCell(int index, bool atEnd) {
  QLineEdit *field = fields_[index];
  field->setFocus();
  field->setCursorPosition(atEnd ? int(field->text().size()) : 0);
}
// Typed or pasted characters fill this cell and run on into the next editable ones; a whole pasted key fills every
// editable group from its own place.
void KeyInput::distribute(int from, const QString &typed) {
  QString chars = keyCharacters(typed);
  const int total = int(fields_.size()) * length_;
  if (chars.size() >= total) {
    for (int i : editable_) fields_[i]->setText(chars.mid(i * length_, length_));
    focusCell(editable_.last(), true);
    return;
  }
  int k = int(editable_.indexOf(from));
  if (k < 0) return;
  fields_[from]->setText(chars.left(length_));
  chars = chars.mid(length_);
  int last = from;
  while (!chars.isEmpty() && ++k < editable_.size()) {
    last = editable_[k];
    fields_[last]->setText(chars.left(length_));
    chars = chars.mid(length_);
  }
  if (fields_[last]->text().size() >= length_ && editable_.indexOf(last) + 1 < editable_.size()) focusCell(editable_[editable_.indexOf(last) + 1], false);
  else focusCell(last, true);
}

#ifdef ROOM237_QA
void HardeningSetup::qaSubmit(const QString &password, const QString &repeat, const QString &decoy, const QStringList &kinds) {
  password_->setText(password);
  repeat_->setText(repeat);
  if (decoy_) decoy_->setText(decoy);
  for (const auto &[choice, toggle] : factors_) toggle->setChecked(kinds.contains(choice.kind));
  submit();
}
QString HardeningSetup::qaDone(const QStringList &groups) {
  showCheck();
  const QStringList all = recoveryKey_.split(QLatin1Char('-'));
  checkInput_->setGroups(groups.size() == 2 ? groups : QStringList{all.value(groupA_), all.value(groupB_)});
  check();
  return checkError_->isHidden() ? QString() : checkError_->text();
}void EncryptOffer::qaChoose(const QString &choice) {
  if (TextButton *button = choices_.value(choice)) button->click();
}
void VaultUnlock::qaSubmit(const QString &password, const QString &recoveryKey, bool useRecovery, bool secondary) {
  if (useRecovery != recovery_) useRecovery_->click();
  password_->setText(password);
  recoveryKey_->setGroups(recoveryKey.split(QLatin1Char('-')));
  (secondary ? secondary_ : primary_)->click();
}
QJsonObject VaultUnlock::qaState() const {
  return {{QStringLiteral("visible"), isVisible()}, {QStringLiteral("root"), root_}, {QStringLiteral("resumable"), resumable_}, {QStringLiteral("recovery"), recovery_}, {QStringLiteral("busy"), busy_},
          {QStringLiteral("passwordShown"), !password_->isHidden()}, {QStringLiteral("error"), error_->isHidden() ? QString() : error_->text()}, {QStringLiteral("factors"), QJsonArray::fromStringList(kinds_)}};
}
#endif
