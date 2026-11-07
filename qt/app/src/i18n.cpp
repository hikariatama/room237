#include "i18n.h"
#include <QHash>
static QString gLanguage = QStringLiteral("en");
static const QHash<QByteArray, QString> &table() {
  static const QHash<QByteArray, QString> strings = [] {
    QHash<QByteArray, QString> out{
#include "generated/i18n_table.inc"
    };
    return out;
  }();
  return strings;
}
static QString pluralForm(qint64 count) {
  if (gLanguage == QLatin1String("ru")) {
    const qint64 a = qAbs(count) % 100, b = a % 10;
    if (a >= 11 && a <= 14) return QStringLiteral("many");
    if (b == 1) return QStringLiteral("one");
    if (b >= 2 && b <= 4) return QStringLiteral("few");
    return QStringLiteral("many");
  }
  return count == 1 ? QStringLiteral("one") : QStringLiteral("other");
}
namespace I18n {
void setLanguage(const QString &language) { gLanguage = language == QLatin1String("ru") ? language : QStringLiteral("en"); }
QString language() { return gLanguage; }
static QString lookup(const QByteArray &key) {
  const auto &strings = table();
  auto it = strings.constFind(gLanguage.toLatin1() + '|' + key);
  if (it != strings.cend()) return *it;
  it = strings.constFind("en|" + key);
  return it != strings.cend() ? *it : QString::fromLatin1(key);
}
static QString fill(QString text, std::initializer_list<std::pair<const char *, QString>> values) {
  for (const auto &[name, value] : values) text.replace(QStringLiteral("{{%1}}").arg(QLatin1String(name)), value);
  return text;
}
QString t(const char *key, std::initializer_list<std::pair<const char *, QString>> values) { return fill(lookup(key), values); }
QString n(const char *key, qint64 count, std::initializer_list<std::pair<const char *, QString>> values) {
  const QByteArray base(key);
  const auto &strings = table();
  QByteArray form = base + '#' + pluralForm(count).toLatin1();
  if (!strings.contains(gLanguage.toLatin1() + '|' + form)) form = base + "#other";
  return fill(lookup(form), values).replace(QLatin1String("{{count}}"), QString::number(count));
}
}
