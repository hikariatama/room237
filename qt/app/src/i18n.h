#pragma once
// English and Russian UI strings from generated/i18n_table.inc (maintained by hand).
#include <QString>
#include <initializer_list>
#include <utility>
namespace I18n {
void setLanguage(const QString &language);
QString language();
// {{name}} placeholders are replaced from values; a key with plural forms selects one by count.
QString t(const char *key, std::initializer_list<std::pair<const char *, QString>> values = {});
QString n(const char *key, qint64 count, std::initializer_list<std::pair<const char *, QString>> values = {});
}
using I18n::t;
