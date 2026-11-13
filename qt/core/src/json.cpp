#include "json.h"

#include "fs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace core::json {

QString string(const QString &value) {
  QString out;
  out.reserve(value.size() + 2);
  out += QLatin1Char('"');
  for (QChar c : value) {
    const ushort u = c.unicode();
    switch (u) {
    case '"': out += QLatin1String("\\\""); break;
    case '\\': out += QLatin1String("\\\\"); break;
    case '\n': out += QLatin1String("\\n"); break;
    case '\r': out += QLatin1String("\\r"); break;
    case '\t': out += QLatin1String("\\t"); break;
    case '\b': out += QLatin1String("\\b"); break;
    case '\f': out += QLatin1String("\\f"); break;
    default:
      if (u < 0x20) out += QStringLiteral("\\u%1").arg(u, 4, 16, QLatin1Char('0'));
      else out += c;
    }
  }
  out += QLatin1Char('"');
  return out;
}

template <typename T> static QString shortest(T value, int maxDigits) {
  if (!std::isfinite(value)) return QStringLiteral("null");
  char buffer[64];
  for (int digits = 1; digits <= maxDigits; ++digits) {
    std::snprintf(buffer, sizeof buffer, "%.*g", digits, double(value));
    if (T(std::strtod(buffer, nullptr)) == value) break;
  }
  QString text = QString::fromLatin1(buffer);
  // ryu spells exponents without padding or a plus sign: 1e-7, 1e20.
  const int e = text.indexOf(QLatin1Char('e'));
  if (e >= 0) {
    QString mantissa = text.left(e), exponent = text.mid(e + 1);
    const bool negative = exponent.startsWith(QLatin1Char('-'));
    exponent.remove(QLatin1Char('+')).remove(QLatin1Char('-'));
    while (exponent.size() > 1 && exponent.startsWith(QLatin1Char('0'))) exponent.remove(0, 1);
    return mantissa + QLatin1Char('e') + (negative ? QStringLiteral("-") : QString()) + exponent;
  }
  if (!text.contains(QLatin1Char('.'))) text += QLatin1String(".0");
  return text;
}
QString float32(float value) { return shortest(value, 9); }
QString float64(double value) { return shortest(value, 17); }
QString boolean(bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); }

Object &Object::raw(const char *key, const QString &json) {
  fields_.emplace_back(QString::fromLatin1(key), json);
  return *this;
}
QString Object::compact() const {
  QString out = QStringLiteral("{");
  bool first = true;
  for (const auto &[key, value] : fields_) {
    if (!first) out += QLatin1Char(',');
    first = false;
    out += string(key) + QLatin1Char(':') + value;
  }
  return out + QLatin1Char('}');
}
QString Object::pretty(int) const { return prettify(compact()); }

QString prettify(const QString &compact) {
  QString out;
  int depth = 0;
  bool inString = false, escaped = false;
  auto newline = [&] { out += QLatin1Char('\n') + QString(depth * 2, QLatin1Char(' ')); };
  for (int i = 0; i < compact.size(); ++i) {
    const QChar c = compact[i];
    if (inString) {
      out += c;
      if (escaped) escaped = false;
      else if (c == QLatin1Char('\\')) escaped = true;
      else if (c == QLatin1Char('"')) inString = false;
      continue;
    }
    switch (c.unicode()) {
    case '"': inString = true; out += c; break;
    case '{':
    case '[': {
      const QChar close = c == QLatin1Char('{') ? QLatin1Char('}') : QLatin1Char(']');
      if (i + 1 < compact.size() && compact[i + 1] == close) {
        out += c;
        out += close;
        ++i;
        break;
      }
      out += c;
      ++depth;
      newline();
      break;
    }
    case '}':
    case ']': --depth; newline(); out += c; break;
    case ',': out += c; newline(); break;
    case ':': out += QLatin1String(": "); break;
    default: out += c;
    }
  }
  return out;
}

static QString kind(const QJsonValue &value) {
  switch (value.type()) {
  case QJsonValue::Null: return QStringLiteral("null");
  case QJsonValue::Bool: return QStringLiteral("boolean");
  case QJsonValue::Double: return QStringLiteral("number");
  case QJsonValue::String: return QStringLiteral("string");
  case QJsonValue::Array: return QStringLiteral("sequence");
  case QJsonValue::Object: return QStringLiteral("map");
  default: return QStringLiteral("missing");
  }
}
quint64 unsignedField(const QJsonValue &value, const char *field, quint64 max) {
  if (value.isUndefined()) fail(QStringLiteral("missing field `%1`").arg(QLatin1String(field)));
  const double d = value.toDouble(-1);
  if (!value.isDouble() || d < 0 || d != std::floor(d) || d > double(max)) fail(QStringLiteral("%1: invalid type: %2, expected an unsigned integer").arg(QLatin1String(field), kind(value)));
  return quint64(d);
}
qint64 signedField(const QJsonValue &value, const char *field) {
  if (value.isUndefined()) fail(QStringLiteral("missing field `%1`").arg(QLatin1String(field)));
  const double d = value.toDouble();
  if (!value.isDouble() || d != std::floor(d)) fail(QStringLiteral("%1: invalid type: %2, expected an integer").arg(QLatin1String(field), kind(value)));
  return value.toInteger();
}
bool boolField(const QJsonValue &value, const char *field) {
  if (value.isUndefined()) fail(QStringLiteral("missing field `%1`").arg(QLatin1String(field)));
  if (!value.isBool()) fail(QStringLiteral("%1: invalid type: %2, expected a boolean").arg(QLatin1String(field), kind(value)));
  return value.toBool();
}
QString stringField(const QJsonValue &value, const char *field) {
  if (value.isUndefined()) fail(QStringLiteral("missing field `%1`").arg(QLatin1String(field)));
  if (!value.isString()) fail(QStringLiteral("%1: invalid type: %2, expected a string").arg(QLatin1String(field), kind(value)));
  return value.toString();
}
double floatField(const QJsonValue &value, const char *field) {
  if (value.isUndefined()) fail(QStringLiteral("missing field `%1`").arg(QLatin1String(field)));
  if (!value.isDouble()) fail(QStringLiteral("%1: invalid type: %2, expected a float").arg(QLatin1String(field), kind(value)));
  return value.toDouble();
}

}  // namespace core::json
