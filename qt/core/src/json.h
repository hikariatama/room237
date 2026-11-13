#pragma once
// JSON text in serde_json's layout. Stored records and version stamps written by earlier releases compare by their
// exact text, so field order, number spelling and escaping follow that encoder.
#include <QJsonValue>
#include <QString>
#include <optional>
#include <vector>

namespace core::json {

QString string(const QString &value);
QString float32(float value);
QString float64(double value);
QString boolean(bool value);

// Builds {"a":1,"b":"x"} in insertion order; pretty output uses two-space indents like to_string_pretty.
class Object {
public:
  Object &raw(const char *key, const QString &json);
  Object &str(const char *key, const QString &value) { return raw(key, string(value)); }
  Object &str(const char *key, const std::optional<QString> &value) { return raw(key, value ? string(*value) : QStringLiteral("null")); }
  Object &num(const char *key, qint64 value) { return raw(key, QString::number(value)); }
  Object &num(const char *key, const std::optional<qint64> &value) { return raw(key, value ? QString::number(*value) : QStringLiteral("null")); }
  Object &unum(const char *key, quint64 value) { return raw(key, QString::number(value)); }
  Object &flag(const char *key, bool value) { return raw(key, boolean(value)); }
  QString compact() const;
  QString pretty(int indent = 0) const;
  bool empty() const { return fields_.empty(); }
private:
  std::vector<std::pair<QString, QString>> fields_;
  std::vector<bool> nested_;
  friend class Array;
};
// Reformats compact serde-style JSON text with two-space indentation.
QString prettify(const QString &compact);

// Strict readers in the manner of serde: wrong types throw core::Error with the field name.
quint64 unsignedField(const QJsonValue &value, const char *field, quint64 max);
qint64 signedField(const QJsonValue &value, const char *field);
bool boolField(const QJsonValue &value, const char *field);
QString stringField(const QJsonValue &value, const char *field);
double floatField(const QJsonValue &value, const char *field);

}  // namespace core::json
