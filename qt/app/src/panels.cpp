#include "panels.h"
#include "i18n.h"
#include "platform.h"
#include <QHBoxLayout>
#include <QFrame>
#include <QIntValidator>
#include <QStackedLayout>
#include <QSvgRenderer>
#include <QJsonArray>
#include <QKeyEvent>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
static QLabel *label(const QString &text, int size, QFont::Weight weight, const char *color, QWidget *parent = nullptr) {
  // Line heights: 16px for 12px text, 20px for 14px text.
  auto *out = new QLabel(size == 12 || size == 14 ? QStringLiteral("<div style='line-height:%1%'>%2</div>").arg(size == 12 ? 112 : 120).arg(text.toHtmlEscaped()) : text, parent);
  out->setFont(Theme::font(size, weight));
  out->setStyleSheet(QStringLiteral("color:%1;background:transparent").arg(QLatin1String(color)));
  out->setWordWrap(true);
  return out;
}
TextPrompt::TextPrompt(QWidget *host, const QString &title, const QString &actionText, const QString &initial, std::function<void(QString)> accepted) : Popover(host) {
  auto *layout = new QVBoxLayout(content_);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(12);
  layout->addWidget(label(title, 14, QFont::DemiBold, "#ffffff"));
  field_ = new TextField(initial);
  field_->setFont(Theme::font(16));  // The style sheet's 16px; the font carries that size's tracking.
  field_->setPlaceholderText(t("album.placeholder"));
  field_->selectAll();
  layout->addWidget(field_);
  auto *buttons = new QHBoxLayout;
  buttons->addStretch();
  auto *cancel = new TextButton(t("common.cancel"), TextButton::Outline);
  accept_ = new TextButton(actionText, TextButton::Primary);
  cancel->setSmall(true);
  accept_->setSmall(true);
  accept_->setMinimumWidth(100);
  buttons->setSpacing(8);
  buttons->addWidget(cancel);
  buttons->addWidget(accept_);
  layout->addLayout(buttons);
  setPanelSize({288, 150});
  connect(cancel, &QAbstractButton::clicked, this, &Popover::animateClose);
  const auto submit = [this, accepted = std::move(accepted)] {
    const QString name = field_->text().trimmed();
    if (!name.isEmpty()) accepted(name);
  };
  connect(accept_, &QAbstractButton::clicked, this, submit);
  connect(field_, &QLineEdit::returnPressed, this, submit);
  accept_->setEnabled(!initial.trimmed().isEmpty());
  connect(field_, &QLineEdit::textChanged, this, [this](const QString &text) { accept_->setEnabled(!text.trimmed().isEmpty()); });
  connect(this, &Popover::closed, this, &QObject::deleteLater);
  QTimer::singleShot(0, field_, qOverload<>(&QWidget::setFocus));
}
void TextPrompt::setBusy(bool busy) { accept_->setBusy(busy); }
ConfirmPanel::ConfirmPanel(QWidget *host, const QString &title, const QString &description, const QString &actionText, std::function<void()> accepted, bool dangerous, Surface surface) : Popover(host, surface) {
  auto *layout = new QVBoxLayout(content_);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(12);
  heading_ = label(title, 14, QFont::DemiBold, "#ffffff");
  detail_ = label(description, 12, QFont::Normal, "#636363");
  layout->addWidget(heading_);
  layout->addWidget(detail_);
  auto *buttons = new QHBoxLayout;
  buttons->addStretch();
  buttons->setSpacing(8);
  auto *cancel = new TextButton(t("common.cancel"), TextButton::Outline);
  accept_ = new TextButton(actionText, dangerous ? TextButton::Danger : TextButton::Primary);
  auto *accept = accept_;
  cancel->setSmall(true);
  accept->setSmall(true);
  buttons->addWidget(cancel);
  buttons->addWidget(accept);
  layout->addLayout(buttons);
  setFixedPanelWidth(288);
  connect(cancel, &QAbstractButton::clicked, this, &Popover::animateClose);
  connect(accept, &QAbstractButton::clicked, this, [this, accepted = std::move(accepted)] {
    closeNow();
    accepted();
  });
  connect(this, &Popover::closed, this, &QObject::deleteLater);
}
void ConfirmPanel::setActionIcon(const QByteArray &icon) {
  accept_->setIcon(icon);
  accept_->setFixedWidth(accept_->sizeHint().width());
}
// A title, a warning and one destructive row.
DeleteSubmenu::DeleteSubmenu(QWidget *host, const QString &title, const QString &warning, const QString &action, std::function<void()> accepted) : Popover(host, ContextSurface) {
  auto *layout = new QVBoxLayout(content_);
  layout->setContentsMargins(3, 3, 3, 3);
  layout->setSpacing(0);
  auto *heading = label(title, 14, QFont::DemiBold, "#ffffff");
  heading->setContentsMargins(12, 8, 12, 4);
  auto *detail = label(warning, 12, QFont::Normal, "#636363");
  detail->setContentsMargins(12, 0, 12, 8);
  auto *row = new TextButton(action, TextButton::Ghost);
  row->setIcon("trash");
  row->setIconInk(Theme::red400);
  row->setRowColors(Theme::red400, QColor(0xfb, 0x2c, 0x36, 38));
  row->setContentInset(12);
  row->setRadius(Theme::kRadiusXl);
  row->setFont(Theme::font(14));
  layout->addWidget(heading);
  layout->addWidget(detail);
  layout->addWidget(row);
  const int inner = 224 - 8;  // heightForWidth includes the label's margins.
  setPanelSize({224, 3 + heading->heightForWidth(inner) + detail->heightForWidth(inner) + 36 + 3 + 2});
  connect(row, &QAbstractButton::clicked, this, [this, accepted = std::move(accepted)] {
    closeNow();
    accepted();
  });
  connect(this, &Popover::closed, this, &QObject::deleteLater);
}
// Rows, a divider, and a confirm page in the same popover.
AlbumActionsPanel::AlbumActionsPanel(QWidget *host, const QString &album, const QByteArray &revealIcon, std::function<void()> reveal, std::function<void()> copyPath, std::function<void()> remove) : Popover(host) {
  auto *stack = new QStackedLayout(content_);
  auto *menu = new QWidget;
  auto *rows = new QVBoxLayout(menu);
  rows->setContentsMargins(6, 6, 6, 6);
  rows->setSpacing(0);
  const auto row = [&](const QString &text, const QByteArray &icon, bool danger) {
    auto *button = new TextButton(text, TextButton::Ghost);
    button->setIcon(icon);
    button->setContentInset(10);
    button->setRadius(Theme::kRadiusXl);
    button->setFont(Theme::font(14));
    if (danger) {
      button->setIconInk(Theme::red400);
      button->setRowColors(Theme::red400, QColor(0xfb, 0x2c, 0x36, 26));
    } else {
      button->setRowColors(Theme::text, QColor(255, 255, 255, 20));
    }
    rows->addWidget(button);
    return button;
  };
  auto *first = row(t("contextMenu.revealIn", {{"fileManager", Platform::fileManagerName()}}), revealIcon, false);
  auto *second = row(t("contextMenu.copyPath"), "link", false);
  rows->addSpacing(4);
  auto *line = new QFrame;
  line->setFixedHeight(1);
  line->setStyleSheet(QStringLiteral("background:rgba(255,255,255,20);border:none"));
  rows->addWidget(line);
  rows->addSpacing(4);
  auto *third = row(t("album.deleteTitle") + QStringLiteral("…"), "trash", true);
  stack->addWidget(menu);
  auto *confirm = new QWidget;
  auto *column = new QVBoxLayout(confirm);
  column->setContentsMargins(14, 14, 14, 14);
  column->setSpacing(12);
  auto *question = label(t("album.deleteConfirm", {{"album", album}}), 14, QFont::Normal, "#ffffff");
  auto *warning = label(t("album.deleteWarning"), 12, QFont::Normal, "#636363");
  column->addWidget(question);
  column->addWidget(warning);
  auto *buttons = new QHBoxLayout;
  buttons->setSpacing(8);
  auto *cancel = new TextButton(t("common.cancel"), TextButton::Outline);
  cancel->setSmall(true);
  cancel->setIcon("arrow-left");
  auto *accept = new TextButton(t("common.delete"), TextButton::Danger);
  accept->setSmall(true);
  accept->setIcon("trash");
  buttons->addWidget(cancel, 1);
  buttons->addWidget(accept, 1);
  column->addLayout(buttons);
  stack->addWidget(confirm);
  const QSize menuSize(256, 6 + 36 * 3 + 9 + 6 + 2);
  setPanelSize(menuSize);
  connect(first, &QAbstractButton::clicked, this, [this, reveal = std::move(reveal)] { closeNow(); reveal(); });
  connect(second, &QAbstractButton::clicked, this, [this, copyPath = std::move(copyPath)] { closeNow(); copyPath(); });
  connect(third, &QAbstractButton::clicked, this, [this, stack, question, warning] {
    stack->setCurrentIndex(1);
    const int inner = 256 - 2 - 28;
    setPanelSize({256, 14 + question->heightForWidth(inner) + 12 + warning->heightForWidth(inner) + 12 + 30 + 14 + 2});
  });
  connect(cancel, &QAbstractButton::clicked, this, [this, stack, menuSize] {
    stack->setCurrentIndex(0);
    setPanelSize(menuSize);
  });
  connect(accept, &QAbstractButton::clicked, this, [this, remove = std::move(remove)] { closeNow(); remove(); });
  connect(this, &Popover::closed, this, &QObject::deleteLater);
}
void ConfirmPanel::setFixedPanelWidth(int width) {
  const int inner = width - 34;
  setPanelSize({width, 16 + heading_->heightForWidth(inner) + 12 + detail_->heightForWidth(inner) + 12 + 30 + 16 + 2});
}
DatePanel::DatePanel(QWidget *host, const QDateTime &value, std::function<void(QDateTime)> accepted) : Popover(host, ContextSurface) {
  auto *layout = new QVBoxLayout(content_);
  // The footer buttons take the menu item's text size and height.
  layout->setContentsMargins(8, 8, 8, 12);
  layout->setSpacing(12);
  auto *picker = new DateTimePicker;
  picker->setDateTime(value);
  layout->addWidget(picker, 0, Qt::AlignHCenter);
  auto *buttons = new QHBoxLayout;
  buttons->addStretch();
  auto *cancel = new TextButton(t("contextMenu.cancel"), TextButton::Outline);
  auto *save = new TextButton(t("contextMenu.save"), TextButton::Primary);
  for (TextButton *button : {cancel, save}) {
    button->setFont(Theme::font(14));
    button->setFixedHeight(36);
    button->setFixedWidth(button->sizeHint().width());
  }
  buttons->setSpacing(8);
  buttons->addWidget(cancel);
  buttons->addWidget(save);
  layout->addLayout(buttons);
  const auto fit = [this, picker] { setPanelSize({298, 8 + picker->preferredHeight() + 12 + 36 + 12 + 2}); };
  fit();
  connect(picker, &DateTimePicker::heightChanged, this, fit);
  // Nothing to save until the date or the time differs from the file's.
  save->setEnabled(false);
  connect(picker, &DateTimePicker::edited, save, [save, picker, value] { save->setEnabled(picker->dateTime() != value); });
  connect(cancel, &QAbstractButton::clicked, this, &Popover::animateClose);
  connect(save, &QAbstractButton::clicked, this, [this, picker, accepted = std::move(accepted)] {
    const QDateTime when = picker->dateTime();
    for (Menu *menu : parentWidget()->findChildren<Menu *>(Qt::FindDirectChildrenOnly)) menu->closeNow();
    closeNow();
    accepted(when);
  });
  connect(this, &Popover::closed, this, &QObject::deleteLater);
}
HotkeyPanel::HotkeyPanel(QWidget *host, bool albumSelected, bool hasSelection) : Popover(host), album_(albumSelected), selection_(hasSelection) {
  // Groups of a small title and rows of a label and a key cap.
  setPanelSize({288, 16 + 20 + 12 + 146 + 12 + 94 + 12 + 94 + 16 + 2});
  content_->setAttribute(Qt::WA_TransparentForMouseEvents);
  connect(this, &Popover::closed, this, &QObject::deleteLater);
}
// Icons for modifiers, arrows and the mouse; other keys as written.
static void paintKeys(QPainter &p, QStringList keys, int right, int y) {
#ifdef Q_OS_MACOS
  const bool mac = true;
#else
  const bool mac = false;
#endif
  if (mac && keys == QStringList{QStringLiteral("delete")}) keys = {QStringLiteral("meta"), QStringLiteral("backspace")};
  static const QHash<QString, QByteArray> icons{{QStringLiteral("shift"), "arrow-big-up"}, {QStringLiteral("arrowleft"), "arrow-left"}, {QStringLiteral("arrowright"), "arrow-right"}, {QStringLiteral("click"), "mouse-left-click"}, {QStringLiteral("rightclick"), "mouse-right-click"}, {QStringLiteral("delete"), "http-delete"}, {QStringLiteral("backspace"), "backspace"}};
  QFont font = Theme::font(10, QFont::Medium);
  p.setFont(font);
  int x = right;
  for (int k = int(keys.size()) - 1; k >= 0; k--) {
    QByteArray icon = icons.value(keys[k]);
    QString text = keys[k];
    if (keys[k] == QLatin1String("meta")) {
      if (mac) icon = "command";
      else text = QStringLiteral("Ctrl");
    }
    const int w = icon.isEmpty() ? qMax(20, p.fontMetrics().horizontalAdvance(text) + 8) : 20;
    x -= w;
    const QRect key(x, y, w, 20);
    p.setPen(QPen(QColor(255, 255, 255, 26), 1));
    p.setBrush(QColor(255, 255, 255, 20));
    p.drawRoundedRect(QRectF(key).adjusted(0.5, 0.5, -0.5, -0.5), Theme::kRadiusSm, Theme::kRadiusSm);
    if (!icon.isEmpty()) {
      Theme::drawIcon(p, icon, QRectF(key.center().x() - 5.5, key.center().y() - 5.5, 12, 12), QColor(255, 255, 255, 204));
    } else {
      p.setPen(QColor(255, 255, 255, 204));
      p.drawText(key, Qt::AlignCenter, text);
    }
    x -= 4;
  }
}
void HotkeyPanel::paintEvent(QPaintEvent *event) {
  Popover::paintEvent(event);
  if (fadeRunning()) return;
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRect area = panelRect().adjusted(17, 17, -17, -15);
  struct Hint { QStringList keys; QString text; };
  struct Group { QString title; bool enabled; QVector<Hint> hints; };
  const auto key = [](const char *name) { return QString::fromLatin1(name); };
  const QVector<Group> groups = {
      {t("hotkeys.group.browse"), album_, {{{key("meta"), key("click")}, t("hotkeys.selectPhoto")}, {{key("shift"), key("click")}, t("hotkeys.selectRange")}, {{key("meta"), key("a")}, t("hotkeys.selectAll")}, {{key("meta"), key("v")}, t("hotkeys.pasteFromClipboard")}, {{key("rightclick")}, t("hotkeys.moreActions")}}},
      {t("hotkeys.group.selection"), selection_, {{{key("meta"), key("c")}, t("hotkeys.copyToClipboard")}, {{key("delete")}, t("hotkeys.deleteSelection")}, {{key("meta"), key("d")}, t("hotkeys.clearSelection")}}},
      {t("hotkeys.group.viewer"), true, {{{key("arrowleft"), key("arrowright")}, t("hotkeys.navigate")}, {{key("meta"), key("+")}, t("hotkeys.zoom")}, {{key("esc")}, t("hotkeys.close")}}},
  };
  p.setPen(Theme::text);
  p.setFont(Theme::font(14, QFont::DemiBold));
  p.drawText(QRect(area.left(), area.top(), area.width(), 20), Qt::AlignLeft | Qt::AlignVCenter, t("hotkeys.title"));
  int y = area.top() + 32;
  for (const Group &group : groups) {
    p.setOpacity(group.enabled ? 1 : 0.45);
    p.setPen(Theme::muted);
    p.setFont(Theme::font(12, QFont::Medium));
    p.drawText(QRect(area.left(), y, area.width(), 16), Qt::AlignLeft | Qt::AlignVCenter, group.title);
    y += 22;
    for (const Hint &hint : group.hints) {
      p.setPen(Theme::text);
      p.setFont(Theme::font(14));
      p.drawText(QRect(area.left(), y, area.width() - 70, 20), Qt::AlignLeft | Qt::AlignVCenter, hint.text);
      paintKeys(p, hint.keys, area.right() + 1, y);
      y += 26;
    }
    y += 6;
  }
}
static QFrame *hairline() {
  auto *line = new QFrame;
  line->setFixedHeight(1);
  line->setStyleSheet(QStringLiteral("background:rgba(255,255,255,15);border:none"));
  return line;
}
// Title row, divided rows and a footer row for advanced settings.
SettingsPanel::SettingsPanel(QWidget *host, SettingsCallbacks c) : Popover(host) {
  auto *layout = new QVBoxLayout(content_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *titleRow = new QHBoxLayout;
  titleRow->setContentsMargins(16, 14, 16, 4);
  auto *title = label(t("settings.title"), 16, QFont::DemiBold, "#ffffff");
  title->setWordWrap(false);
  titleRow->addWidget(title);
  QLabel *version = label(t("settings.version", {{"version", c.version}}), 12, QFont::Normal, "#636363");
  version->setWordWrap(false);
  version->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  titleRow->addWidget(version, 1);
  layout->addLayout(titleRow);
  auto *body = new QVBoxLayout;
  body->setContentsMargins(16, 0, 16, 0);
  body->setSpacing(0);
  int height = 14 + 24 + 4;
  bool first = true;
  const auto section = [&](QLayout *inner, int rowHeight) {
    if (!first) {
      body->addWidget(hairline());
      height += 1;
    }
    first = false;
    auto *pad = new QVBoxLayout;
    pad->setContentsMargins(0, 8, 0, 8);
    pad->addLayout(inner);
    body->addLayout(pad);
    height += rowHeight + 16;
  };
  if (c.showRootControls) {
    auto *row = new QHBoxLayout;
    auto *root = new TextButton(t("settings.changeRoot"), TextButton::Outline);
    root->setIcon("folder-open");
    root->setAlignLeft(true);
    row->addWidget(root, 1);
    section(row, 36);
    connect(root, &QAbstractButton::clicked, this, [this, f = c.changeRoot] { closeNow(); f(); });
    if (c.privacyEnabled()) {
      auto *block = new QVBoxLayout;
      block->setSpacing(6);
      block->addWidget(label(t("settings.decoyGallery"), 12, QFont::Medium, "#636363"));
      auto *decoyRow = new QHBoxLayout;
      decoyRow->setSpacing(6);
      const QString decoy = c.decoyRoot();
      auto *pick = new TextButton(decoy.isEmpty() ? t("settings.pickDecoy") : decoy, TextButton::Outline);
      pick->setIcon("folder");
      pick->setAlignLeft(true);
      pick->setToolTip(decoy);
      decoyRow->addWidget(pick, 1);
      connect(pick, &QAbstractButton::clicked, this, [this, f = c.pickDecoy] { closeNow(); f(); });
      if (!decoy.isEmpty()) {
        auto *clear = new IconButton("x", t("settings.clearDecoy"), 36);
        decoyRow->addWidget(clear);
        connect(clear, &QAbstractButton::clicked, this, [this, f = c.clearDecoy] { closeNow(); f(); });
      }
      block->addLayout(decoyRow);
      section(block, 18 + 6 + 36);
    }
  }
  const auto rowWith = [&](const QString &text, const QString &hint, QWidget *control) {
    auto *row = new QHBoxLayout;
    row->setSpacing(16);
    auto *texts = new QVBoxLayout;
    texts->setSpacing(0);
    auto *name = label(text, 14, QFont::Normal, "#ffffff");
    texts->addWidget(name);
    int rowHeight = name->heightForWidth(320 - 32 - 16 - control->sizeHint().width());
    if (!hint.isEmpty()) {
      auto *helper = label(hint, 12, QFont::Normal, "#636363");
      texts->addWidget(helper);
      rowHeight += helper->heightForWidth(320 - 32 - 16 - control->sizeHint().width());
    } else {
      // A lone label sits on the control's centre line, not at the top of the row.
      texts->insertStretch(0);
      texts->addStretch();
    }
    row->addLayout(texts, 1);
    row->addWidget(control, 0, Qt::AlignVCenter);
    section(row, qMax(rowHeight, control->sizeHint().height()));
  };
  const bool canProtect = Platform::canProtectContent();
  auto *protect = new Switch(t("settings.preventScreenshots"));
  protect->setChecked(c.privacyEnabled() && c.contentProtected());
  protect->setEnabled(c.privacyEnabled() && canProtect);
  connect(protect, &QAbstractButton::toggled, this, c.setContentProtected);
  rowWith(t("settings.preventScreenshots"), !canProtect ? t("settings.screenshotUnavailable") : c.privacyEnabled() ? QString() : t("settings.screenshotHint"), protect);
  // Select trigger with the flag of the language.
  auto *language = new TextButton(c.language() == QLatin1String("ru") ? t("language.ru") : t("language.en"), TextButton::Outline);
  language->setFixedHeight(32);
  language->setFont(Theme::font(14));
  language->setLeadingPixmap(QPixmap(c.language() == QLatin1String("ru") ? QStringLiteral(":/flags/ru.png") : QStringLiteral(":/flags/en.png")), QSize(18, 12));
  language->setTrailingIcon("chevron-down");
  language->setFixedWidth(language->sizeHint().width());
  connect(language, &QAbstractButton::clicked, this, [this, language, set = c.setLanguage] {
    const auto choose = [this, set](const QString &value) { return [this, set, value] { closeNow(); set(value); }; };
    Menu::select(parentWidget(), {{{}, t("language.en"), choose(QStringLiteral("en")), true, false, false, I18n::language() == QLatin1String("en")}, {{}, t("language.ru"), choose(QStringLiteral("ru")), true, false, false, I18n::language() == QLatin1String("ru")}}, hostRect(parentWidget(), language), Qt::AlignRight, 170)->setKeepWithParent(this);
  });
  rowWith(t("settings.language"), {}, language);
  body->addWidget(hairline());
  height += 1;
  layout->addLayout(body);
  auto *footer = new QVBoxLayout;
  footer->setContentsMargins(6, 6, 6, 6);
  footer->setSpacing(0);
  // The icon and the chevron muted.
  auto *advanced = new TextButton(t("settings.advanced"), TextButton::Ghost);
  advanced->setIcon("adjustments");
  advanced->setIconInk(Theme::muted);
  advanced->setTrailingIcon("chevron-right");
  advanced->setContentInset(10);
  advanced->setRadius(Theme::kRadiusXl);
  advanced->setFont(Theme::font(14));
  connect(advanced, &QAbstractButton::clicked, this, [this, run = c.openAdvanced] { closeNow(); run(); });
  footer->addWidget(advanced);
  height += 36;
  layout->addLayout(footer);
  height += 12;
  setPanelSize({320, height + 2});
  connect(this, &Popover::closed, this, &QObject::deleteLater);
}
// A dialog with a title bar, section navigation on the left, one section on the right.
// The flag beside "made with": a heart masking three stripes.
static QPixmap madeWithFlag(qreal ratio) {
  const int side = qRound(16 * ratio);
  QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter p(&image);
  QSvgRenderer heart(QByteArrayLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='white' d='M2 9.49998C2.00002 8.38718 2.33759 7.30056 2.96813 6.38364C3.59867 5.46672 4.49252 4.76264 5.53161 4.36438C6.5707 3.96612 7.70616 3.89242 8.78801 4.15302C9.86987 4.41362 10.8472 4.99626 11.591 5.82398C11.6434 5.87999 11.7067 5.92465 11.7771 5.95518C11.8474 5.98571 11.9233 6.00146 12 6.00146C12.0767 6.00146 12.1526 5.98571 12.2229 5.95518C12.2933 5.92465 12.3566 5.87999 12.409 5.82398C13.1504 4.99088 14.128 4.40335 15.2116 4.13958C16.2952 3.87581 17.4335 3.94833 18.4749 4.34746C19.5163 4.7466 20.4114 5.45343 21.0411 6.37388C21.6708 7.29433 22.0053 8.38474 22 9.49998C22 11.79 20.5 13.5 19 15L13.508 20.313C13.3217 20.527 13.0919 20.6989 12.834 20.8173C12.5762 20.9357 12.296 20.9978 12.0123 20.9996C11.7285 21.0014 11.4476 20.9428 11.1883 20.8277C10.9289 20.7126 10.697 20.5436 10.508 20.332L5 15C3.5 13.5 2 11.8 2 9.49998Z'/></svg>"));
  heart.render(&p, QRectF(0, 0, side, side));
  p.setCompositionMode(QPainter::CompositionMode_SourceIn);
  const qreal unit = side / 24.0;
  p.fillRect(QRectF(0, 3 * unit, side, 8 * unit), QColor(0x08, 0x84, 0x08));
  p.fillRect(QRectF(0, 11 * unit, side, 3 * unit), Qt::white);
  p.fillRect(QRectF(0, 14 * unit, side, 8 * unit), QColor(0xff, 0x08, 0x08));
  p.end();
  QPixmap out = QPixmap::fromImage(image);
  out.setDevicePixelRatio(ratio);
  return out;
}
AdvancedDialog::AdvancedDialog(QWidget *host, CoreClient *core, SettingsCallbacks app, std::function<void(QJsonObject)> applied, std::function<void(int, QString)> notify) : Popover(host, DialogSurface), core_(core), app_(std::move(app)), applied_(std::move(applied)), notify_(std::move(notify)) {
  setModalDim(true);
  setDismissOnOutside(false);
  auto *layout = new QVBoxLayout(content_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *head = new QVBoxLayout;
  head->setContentsMargins(20, 16, 56, 14);
  auto *dialogTitle = label(t("advanced.title"), 16, QFont::DemiBold, "#ffffff");
  dialogTitle->setFixedHeight(24);
  head->addWidget(dialogTitle);
  layout->addLayout(head);
  layout->addWidget(hairline());
  auto *close = new IconButton("x", t("common.close"), 32, content_);
  close->setGeometry(0, 0, 32, 32);
  close->setColors(Theme::muted, Theme::text, Theme::hover);
  connect(close, &QAbstractButton::clicked, this, &Popover::animateClose);
  closeButton_ = close;
  auto *middle = new QHBoxLayout;
  middle->setContentsMargins(0, 0, 0, 0);
  middle->setSpacing(0);
  nav_ = new QVBoxLayout;
  nav_->setContentsMargins(8, 8, 8, 8);
  nav_->setSpacing(2);
  auto *navHolder = new QWidget;
  navHolder->setFixedWidth(224);
  navHolder->setLayout(nav_);
  middle->addWidget(navHolder);
  auto *divider = new QFrame;
  divider->setFixedWidth(1);
  divider->setStyleSheet(QStringLiteral("background:rgba(255,255,255,26);border:none"));
  middle->addWidget(divider);
  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *inner = new QWidget;
  inner->setStyleSheet(QStringLiteral("background:transparent"));
  sections_ = new QVBoxLayout(inner);
  sections_->setContentsMargins(20, 8, 20, 8);
  sections_->setSpacing(0);
  scroll->setWidget(inner);
  scroll->viewport()->setStyleSheet(QStringLiteral("background:transparent"));
  middle->addWidget(scroll, 1);
  layout->addLayout(middle, 1);
  layout->addWidget(hairline());
  auto *foot = new QHBoxLayout;
  foot->setContentsMargins(16, 12, 16, 12);
  foot->setSpacing(8);
  auto *resetAll = new TextButton(t("advanced.resetAll"), TextButton::Ghost);
  auto *cancel = new TextButton(t("advanced.cancel"), TextButton::Outline);
  save_ = new TextButton(t("advanced.save"), TextButton::Primary);
  for (TextButton *button : {resetAll, cancel, save_}) button->setSmall(true);
  foot->addWidget(resetAll);
  foot->addStretch();
  foot->addWidget(cancel);
  foot->addWidget(save_);
  layout->addLayout(foot);
  setPanelSize({qMin(736, host->width() * 92 / 100), qMin(640, host->height() * 85 / 100)});
  connect(cancel, &QAbstractButton::clicked, this, &Popover::animateClose);
  connect(save_, &QAbstractButton::clicked, this, [this] {
    core_->call(QStringLiteral("update_settings"), {{QStringLiteral("settings"), settings_}}, [this](const QJsonValue &value, const QString &error) {
      if (!error.isEmpty()) {
        notify_(1, t("advanced.saveError") + QStringLiteral(": ") + error);
        return;
      }
      notify_(0, t("advanced.saveSuccess"));
      applied_(value.isObject() ? value.toObject() : settings_);
      animateClose();
    });
  });
  connect(resetAll, &QAbstractButton::clicked, this, [this] {
    core_->call(QStringLiteral("reset_settings"), {}, [this](const QJsonValue &value, const QString &error) {
      if (!error.isEmpty()) return notify_(1, error);
      notify_(0, t("advanced.resetDefaults"));
      applied_(value.toObject());
      settings_ = value.toObject();
      showSection(active_);
    });
  });
  connect(this, &Popover::closed, this, &QObject::deleteLater);
  core_->call(QStringLiteral("native_default_settings"), {}, [this](const QJsonValue &defaults, const QString &) {
    defaults_ = defaults.toObject();
    core_->call(QStringLiteral("get_settings"), {}, [this](const QJsonValue &value, const QString &) {
      settings_ = original_ = value.toObject();
      buildNavigation();
      showSection(QStringLiteral("privacy"));
    });
  });
}
void AdvancedDialog::resizeEvent(QResizeEvent *event) {
  Popover::resizeEvent(event);
  if (closeButton_) closeButton_->move(panelRect().width() - 32 - 16, 14);
}
QWidget *AdvancedDialog::row(const QString &text, const QString &helper, QWidget *control, const char *section, const char *key) {
  auto *holder = new QWidget;
  auto *h = new QHBoxLayout(holder);
  h->setContentsMargins(0, 12, 0, 12);
  h->setSpacing(24);
  auto *texts = new QVBoxLayout;
  texts->setSpacing(2);
  auto *title = new QHBoxLayout;
  title->setSpacing(8);
  auto *name = label(text, 14, QFont::Medium, "#ffffff");
  name->setWordWrap(false);
  title->addWidget(name, 0);
  const QJsonValue value = settings_.value(QString::fromLatin1(section)).toObject().value(QString::fromLatin1(key)), standard = defaults_.value(QString::fromLatin1(section)).toObject().value(QString::fromLatin1(key));
  if (value != standard) {
    auto *dot = new QLabel;
    dot->setFixedSize(6, 6);
    dot->setStyleSheet(QStringLiteral("background:#ffb900;border-radius:3px"));
    const QString shown = standard.isBool() ? (standard.toBool() ? t("common.on") : t("common.off")) : standard.isDouble() ? QString::number(standard.toDouble()) : standard.toString();
    dot->setToolTip(t("advanced.defaultValue", {{"value", shown}}));
    title->addWidget(dot, 0, Qt::AlignVCenter);
  }
  title->addStretch();
  texts->addLayout(title);
  auto *hint = label(helper, 12, QFont::Normal, "#636363");
  texts->addWidget(hint);
  h->addLayout(texts, 1);
  h->addWidget(control, 0, Qt::AlignVCenter);
  // The scroll area sizes the page from minimum heights, which do not follow wrapped text by themselves.
  const int controlWidth = control->maximumWidth() < QWIDGETSIZE_MAX ? control->maximumWidth() : control->sizeHint().width();
  const int textWidth = 736 - 2 - 224 - 1 - 40 - 24 - controlWidth;
  holder->setFixedHeight(12 + qMax(20 + 2 + hint->heightForWidth(textWidth), control->sizeHint().height()) + 12);
  return holder;
}
QWidget *AdvancedDialog::numberField(const char *section, const char *key, const char *labelKey, const char *helperKey, int low, int high) {
  auto *edit = new TextField;
  edit->setMetrics(32, 14);
  edit->setFixedWidth(112);
  edit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  edit->setValidator(new QIntValidator(low, high, edit));
  edit->setAccessibleName(t(labelKey));
  const QString sec = QString::fromLatin1(section), k = QString::fromLatin1(key);
  edit->setText(QString::number(settings_.value(sec).toObject().value(k).toInt()));
  connect(edit, &QLineEdit::textEdited, this, [this, sec, k](const QString &text) {
    QJsonObject object = settings_.value(sec).toObject();
    object[k] = text.toInt();
    settings_[sec] = object;
    refreshState();
  });
  return row(t(labelKey), t(helperKey), edit, section, key);
}
QWidget *AdvancedDialog::sliderField(const char *section, const char *key, const char *labelKey, const char *helperKey, int low, int high) {
  const QString sec = QString::fromLatin1(section), k = QString::fromLatin1(key);
  auto *slider = new NumberSlider(low, high, settings_.value(sec).toObject().value(k).toInt(), t(labelKey));
  slider->setFixedWidth(224);
  connect(slider, &NumberSlider::valueChanged, this, [this, sec, k](int value) {
    QJsonObject object = settings_.value(sec).toObject();
    object[k] = value;
    settings_[sec] = object;
    refreshState();
  });
  return row(t(labelKey), t(helperKey), slider, section, key);
}
QWidget *AdvancedDialog::boolField(const char *section, const char *key, const char *labelKey, const char *helperKey) {
  auto *control = new Switch(t(labelKey));
  const QString sec = QString::fromLatin1(section), k = QString::fromLatin1(key);
  control->setChecked(settings_.value(sec).toObject().value(k).toBool());
  connect(control, &QAbstractButton::toggled, this, [this, sec, k](bool value) {
    QJsonObject object = settings_.value(sec).toObject();
    object[k] = value;
    settings_[sec] = object;
    refreshState();
  });
  return row(t(labelKey), t(helperKey), control, section, key);
}
QWidget *AdvancedDialog::enumField(const char *section, const char *key, const char *labelKey, const char *helperKey, const QVector<std::pair<QString, QString>> &options) {
  const QString sec = QString::fromLatin1(section), k = QString::fromLatin1(key);
  auto *button = new TextButton(QString(), TextButton::Outline);
  button->setFixedSize(144, 32);
  button->setFont(Theme::font(14));
  button->setTrailingIcon("chevron-down");
  button->setAlignLeft(true);
  const auto textFor = [options](const QString &value) {
    for (const auto &[id, text] : options)
      if (id == value) return text;
    return value;
  };
  button->setText(textFor(settings_.value(sec).toObject().value(k).toString()));
  connect(button, &QAbstractButton::clicked, this, [this, button, options, sec, k] {
    QVector<MenuItem> items;
    const QString current = settings_.value(sec).toObject().value(k).toString();
    for (const auto &[id, text] : options) {
      items.push_back({{}, text, [this, button, sec, k, id = id, text = text] {
        QJsonObject object = settings_.value(sec).toObject();
        object[k] = id;
        settings_[sec] = object;
        button->setText(text);
        refreshState();
      }, true, false, false, id == current});
    }
    Menu::select(parentWidget(), items, hostRect(parentWidget(), button), Qt::AlignRight, 190)->setKeepWithParent(this);
  });
  return row(t(labelKey), t(helperKey), button, section, key);
}
QWidget *AdvancedDialog::textField(const char *section, const char *key, const char *labelKey, const char *helperKey) {
  const QString sec = QString::fromLatin1(section), k = QString::fromLatin1(key);
  auto *edit = new TextField(settings_.value(sec).toObject().value(k).toString());
  edit->setMetrics(32, 14);
  edit->setFixedWidth(160);
  edit->setAccessibleName(t(labelKey));
  connect(edit, &QLineEdit::textEdited, this, [this, sec, k](const QString &value) {
    QJsonObject object = settings_.value(sec).toObject();
    object[k] = value;
    settings_[sec] = object;
    refreshState();
  });
  return row(t(labelKey), t(helperKey), edit, section, key);
}
void AdvancedDialog::refreshState() {
  if (save_) save_->setEnabled(settings_ != original_);
  // Modified dots in the navigation follow the draft.
  for (auto it = navButtons_.cbegin(); it != navButtons_.cend(); ++it) it.value()->setDot(settings_.value(it.key()) != defaults_.value(it.key()));
}
void AdvancedDialog::buildNavigation() {
  struct Item { const char *key, *title; const char *icon; };
  static const Item items[] = {{"privacy", "advanced.section.privacy", "shield-lock"}, {"duplicates", "advanced.section.duplicates", "copy"}, {"thumbnails", "advanced.section.thumbnails", "photo"}, {"ffmpeg", "advanced.section.ffmpeg", "movie"}, {"preload", "advanced.section.preload", "bolt"}, {"metadata", "advanced.section.metadata", "tags"}, {"album", "advanced.section.album", "folders"}, {"app", "advanced.section.app", "cloud-download"}};
  for (const Item &item : items) {
    auto *button = new TextButton(t(item.title), TextButton::Ghost);
    button->setIcon(item.icon);
    button->setAlignLeft(true);
    button->setRadius(Theme::kRadiusXl);
    button->setFont(Theme::font(14));
    button->setFixedHeight(36);
    button->setCheckable(true);
    button->setNavigationStyle(true);
    const QString key = QString::fromLatin1(item.key);
    navButtons_.insert(key, button);
    nav_->addWidget(button);
    connect(button, &QAbstractButton::clicked, this, [this, key] { showSection(key); });
  }
  nav_->addStretch();
  auto *madeWith = new QHBoxLayout;
  madeWith->setContentsMargins(10, 0, 0, 2);
  madeWith->setSpacing(6);
  auto *flag = new QLabel;
  flag->setPixmap(madeWithFlag(devicePixelRatioF()));
  madeWith->addWidget(flag);
  madeWith->addWidget(label(QStringLiteral("белән эшләнде"), 12, QFont::Normal, "#636363"));
  madeWith->addStretch();
  nav_->addLayout(madeWith);
  auto *link = label(QStringLiteral("hikariatama/room237"), 12, QFont::Normal, "#636363");
  link->setContentsMargins(10, 0, 0, 0);
  auto *license = label(QStringLiteral("GNU AGPLv3"), 10, QFont::Normal, "#4f4f4f");
  license->setContentsMargins(10, 0, 0, 4);
  nav_->addWidget(link);
  nav_->addWidget(license);
}
void AdvancedDialog::showSection(const QString &key) {
  active_ = key;
  while (QLayoutItem *item = sections_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  for (auto it = navButtons_.cbegin(); it != navButtons_.cend(); ++it) it.value()->setChecked(it.key() == key);
  const auto add = [this](QWidget *field, bool first = false) {
    if (!first) {
      auto *line = hairline();
      line->setStyleSheet(QStringLiteral("background:rgba(255,255,255,13);border:none"));
      sections_->addWidget(line);
    }
    sections_->addWidget(field);
  };
  if (key == QLatin1String("privacy")) {
    add(boolField("privacy", "enabled", "advanced.field.privacyFeatures", "advanced.field.privacyFeatures.helper"), true);
    add(boolField("privacy", "lockscreenEnabled", "advanced.field.lockscreen", "advanced.field.lockscreen.helper"));
    add(boolField("privacy", "confirmOpenEnabled", "advanced.field.confirmOpen", "advanced.field.confirmOpen.helper"));
    add(boolField("privacy", "hardening", "advanced.field.hardening", "advanced.field.hardening.helper"));
    // The open root: encrypt it, or remove its encryption.
    const QString state = app_.vaultState ? app_.vaultState() : QString();
    if (!state.isEmpty() && state != QLatin1String("nested")) {
      auto *actions = new QWidget;
      auto *row = new QHBoxLayout(actions);
      row->setContentsMargins(0, 0, 0, 0);
      row->setSpacing(8);
      const auto action = [this, row](const QString &text, TextButton::Kind kind, const QByteArray &icon, const std::function<void()> &run) {
        auto *button = new TextButton(text, kind);
        button->setSmall(true);
        button->setIcon(icon);
        row->addWidget(button);
        connect(button, &QAbstractButton::clicked, this, [this, run] {
          closeNow();
          if (run) run();
        });
      };
      QString helper;
      if (state == QLatin1String("none")) {
        helper = t("advanced.hardening.plain");
        action(t("advanced.hardening.encrypt"), TextButton::Primary, "lock", app_.encryptRoot);
      } else if (state == QLatin1String("encrypted")) {
        helper = t("advanced.hardening.encrypted");
        action(t("advanced.hardening.newFactor"), TextButton::Outline, "fingerprint", app_.replaceFactor);
        action(t("advanced.hardening.decrypt"), TextButton::Danger, "lock-open", app_.decryptRoot);
      } else {
        helper = state == QLatin1String("decrypting") ? t("advanced.hardening.decrypting") : t("advanced.hardening.encrypting");
        if (state == QLatin1String("decrypting")) action(t("advanced.hardening.decrypt"), TextButton::Danger, "lock-open", app_.decryptRoot);
      }
      row->addStretch();
      add(this->row(t("advanced.hardening.root"), helper, actions, "", ""));
    }
  } else if (key == QLatin1String("duplicates")) {
    add(sliderField("duplicates", "threshold", "advanced.field.duplicates.threshold", "advanced.field.duplicates.threshold.helper", 0, 128), true);
    add(enumField("duplicates", "hashSize", "advanced.field.duplicates.hashSize", "advanced.field.duplicates.hashSize.helper", {{QStringLiteral("8x8"), QStringLiteral("8×8")}, {QStringLiteral("16x16"), QStringLiteral("16×16")}, {QStringLiteral("32x32"), QStringLiteral("32×32")}}));
    add(enumField("duplicates", "hashAlg", "advanced.field.duplicates.algorithm", "advanced.field.duplicates.algorithm.helper", {{QStringLiteral("blockhash"), QStringLiteral("Blockhash")}, {QStringLiteral("phash"), t("advanced.option.phash")}, {QStringLiteral("dhash"), t("advanced.option.dhash")}}));
    add(enumField("duplicates", "resizeFilter", "advanced.field.duplicates.resizeFilter", "advanced.field.duplicates.resizeFilter.helper", {{QStringLiteral("nearest"), t("advanced.option.nearest")}, {QStringLiteral("triangle"), t("advanced.option.triangle")}, {QStringLiteral("catmullrom"), QStringLiteral("Catmull-Rom")}, {QStringLiteral("lanczos3"), QStringLiteral("Lanczos3")}}));
    add(boolField("duplicates", "useThumbnailsFirst", "advanced.field.duplicates.useThumbsFirst", "advanced.field.duplicates.useThumbsFirst.helper"));
  } else if (key == QLatin1String("thumbnails")) {
    add(numberField("thumbnails", "maxDim", "advanced.field.thumbs.maxDim", "advanced.field.thumbs.maxDim.helper", 128, 2048), true);
    add(sliderField("thumbnails", "imageWebpQuality", "advanced.field.thumbs.webpQuality", "advanced.field.thumbs.webpQuality.helper", 30, 95));
    add(sliderField("thumbnails", "imageWebpCompressionLevel", "advanced.field.thumbs.webpCompression", "advanced.field.thumbs.webpCompression.helper", 0, 6));
    add(numberField("thumbnails", "videoSeekSeconds", "advanced.field.thumbs.videoSeek", "advanced.field.thumbs.videoSeek.helper", 0, 30));
    add(numberField("thumbnails", "lockPollMs", "advanced.field.thumbs.lockPoll", "advanced.field.thumbs.lockPoll.helper", 5, 250));
  } else if (key == QLatin1String("ffmpeg")) {
    add(sliderField("ffmpeg", "threads", "advanced.field.ffmpeg.threads", "advanced.field.ffmpeg.threads.helper", 1, 32), true);
    add(numberField("ffmpeg", "timeoutSecs", "advanced.field.ffmpeg.timeout", "advanced.field.ffmpeg.timeout.helper", 1, 60));
    add(textField("ffmpeg", "hwaccel", "advanced.field.ffmpeg.hwaccel", "advanced.field.ffmpeg.hwaccel.helper"));
    add(numberField("ffmpeg", "processWaitPollMs", "advanced.field.ffmpeg.processPoll", "advanced.field.ffmpeg.processPoll.helper", 5, 200));
  } else if (key == QLatin1String("preload")) {
    add(sliderField("preload", "thumbWorkers", "advanced.field.preload.thumbWorkers", "advanced.field.preload.thumbWorkers.helper", 1, 32), true);
    add(sliderField("preload", "metaWorkers", "advanced.field.preload.metaWorkers", "advanced.field.preload.metaWorkers.helper", 1, 32));
    add(sliderField("preload", "hashWorkers", "advanced.field.preload.hashWorkers", "advanced.field.preload.hashWorkers.helper", 1, 32));
    add(sliderField("preload", "encryptWorkers", "advanced.field.preload.encryptWorkers", "advanced.field.preload.encryptWorkers.helper", 1, 32));
    add(numberField("preload", "progressEmitMs", "advanced.field.preload.progressEmit", "advanced.field.preload.progressEmit.helper", 50, 1000));
    add(numberField("preload", "thumbHashQueueDelayMs", "advanced.field.preload.hashDelay", "advanced.field.preload.hashDelay.helper", 0, 100));
    add(boolField("preload", "thumbHashRetryOnThumbChange", "advanced.field.preload.hashRetry", "advanced.field.preload.hashRetry.helper"));
  } else if (key == QLatin1String("metadata")) {
    add(numberField("metadata", "ffmpegProbeTimeoutSecs", "advanced.field.metadata.probeTimeout", "advanced.field.metadata.probeTimeout.helper", 1, 60), true);
    add(boolField("metadata", "parseCreationTime", "advanced.field.metadata.parseCreation", "advanced.field.metadata.parseCreation.helper"));
  } else if (key == QLatin1String("app")) {
    // Kept for updates and frame pacing, outside the core settings file.
    auto *update = new Switch(t("settings.autoUpdate"));
    update->setChecked(app_.autoUpdate());
    connect(update, &QAbstractButton::toggled, this, app_.setAutoUpdate);
    add(row(t("settings.autoUpdate"), t("settings.autoUpdateHint"), update, "", ""), true);
    auto *check = new TextButton(t("update.check"), TextButton::Outline);
    check->setSmall(true);
    connect(check, &QAbstractButton::clicked, this, [this] { app_.checkUpdates(); });
    add(row(t("update.check"), t("settings.version", {{"version", app_.version}}), check, "", ""));
    auto *scroll = new Switch(t("settings.scroll60"));
    scroll->setChecked(app_.scroll60());
    connect(scroll, &QAbstractButton::toggled, this, app_.setScroll60);
    add(row(t("settings.scroll60"), t("settings.scroll60Hint"), scroll, "", ""));
  } else {
    add(numberField("album", "renameCleanupDelaySecs", "advanced.field.album.renameDelay", "advanced.field.album.renameDelay.helper", 0, 10), true);
    add(boolField("album", "moveRenameThumbsAndMeta", "advanced.field.album.moveThumbs", "advanced.field.album.moveThumbs.helper"));
  }
  if (settings_.value(key) != defaults_.value(key)) {
    auto *reset = new TextButton(t("advanced.resetSection"), TextButton::Ghost);
    reset->setSmall(true);
    reset->setFixedHeight(28);
    connect(reset, &QAbstractButton::clicked, this, [this, key] {
      settings_[key] = defaults_.value(key);
      showSection(key);
      refreshState();
    });
    auto *holder = new QWidget;
    auto *line = new QHBoxLayout(holder);
    line->setContentsMargins(0, 4, 0, 12);
    line->addWidget(reset);
    line->addStretch();
    sections_->addWidget(holder);
  }
  sections_->addStretch();
  refreshState();
}
