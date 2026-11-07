#include "qa_fence.h"
#include "updater.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
namespace QaFence {
bool enabled() { return qEnvironmentVariableIsSet("ROOM237_QA_PORT"); }
bool inside(const QString &path, const QString &fence, bool allowMissing) {
  // Both the input and the configured boundary must be absolute, without lexical traversal.
  auto absolute = [](const QString &p) {
    return !p.isEmpty() && QDir::isAbsolutePath(p) &&
           !QDir::fromNativeSeparators(p).split('/').contains(QStringLiteral(".."));
  };
  if (!absolute(path) || !absolute(fence)) return false;
  const QFileInfo boundary(fence);
  const QString base = boundary.canonicalFilePath();
  // A filesystem root is never a disposable task boundary. No component-name fallback.
  if (!boundary.isDir() || base.isEmpty() || QDir(base).isRoot()) return false;
  QFileInfo info(path);
  if (!allowMissing && !info.exists()) return false;
  QString existing = info.absoluteFilePath();
  QStringList missing;
  while (!QFileInfo::exists(existing)) {
    const QFileInfo leaf(existing);
    if (leaf.isSymLink() || QDir(existing).isRoot()) return false;
    missing.prepend(leaf.fileName());
    existing = leaf.absolutePath();
  }
  const QFileInfo ancestor(existing);
  if (!missing.isEmpty() && !ancestor.isDir()) return false;
  QString real = ancestor.canonicalFilePath();
  if (real.isEmpty()) return false;
  for (const auto &part : missing) real = QDir(real).filePath(part);
  return real == base || real.startsWith(base + QLatin1Char('/'));
}
bool allows(const QString &path, bool allowMissing) {
  if (!enabled()) return true;
  const bool ok = inside(path, qEnvironmentVariable("ROOM237_QA_FENCE"), allowMissing);
  if (!ok) qWarning("QA fence refused path: %s", qPrintable(path));
  return ok;
}
bool rootsAllowed(const QJsonObject &state) {
  if (!enabled()) return true;
  for (const auto &key : {QStringLiteral("rootDir"), QStringLiteral("decoyRoot")}) {
    const QJsonValue value = state.value(key);
    if (!value.isNull() && !value.isUndefined() && (!value.isString() || (!value.toString().isEmpty() && (!allows(value.toString()) || !QFileInfo(value.toString()).isDir())))) return false;
  }
  return true;
}
bool profileAllowed(const QString &profile, bool legacy) {
  if (!enabled()) return true;
  const QString path = QDir(profile).filePath(legacy ? QStringLiteral("state.json") : QStringLiteral("ui-state.json"));
  if (!allows(path, true)) return false;
  if (!QFileInfo::exists(path)) return !QFileInfo(path).isSymLink();
  if (!allows(path)) return false;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
  if (!legacy) return rootsAllowed(doc.object());
  const QJsonValue stored = doc.object().value(QStringLiteral("room237-storage"));
  if (stored.isUndefined()) return true;
  if (stored.isObject()) return rootsAllowed(stored.toObject());
  if (!stored.isString()) return false;
  const QJsonDocument inner = QJsonDocument::fromJson(stored.toString().toUtf8(), &error);
  return error.error == QJsonParseError::NoError && inner.isObject() && rootsAllowed(inner.object());
}
static bool pathValueAllowed(const QString &key, const QJsonValue &value, bool bareRoot = false) {
  if (!value.isString()) return false;
  const QString path = value.toString();
  // The duplicate commands name a group's first file as root: a bare name inside the separately checked dir.
  if (bareRoot && key == QLatin1String("root") && !path.isEmpty() && path != QLatin1String(".") && path != QLatin1String("..") && !path.contains('/') && !path.contains('\\')) return true;
  // A file can vanish while a request is on its way and the core answers that itself; the real location still decides.
  return allows(path, true);
}
static bool valueAllowed(const QJsonValue &value, bool bareRoot = false) {
  if (value.isArray()) {
    for (const auto &item : value.toArray()) if (!valueAllowed(item)) return false;
  } else if (value.isObject()) {
    const auto object = value.toObject();
    const QSet<QString> paths{QStringLiteral("rootDir"), QStringLiteral("directory"), QStringLiteral("dir"), QStringLiteral("root"), QStringLiteral("albumPath"), QStringLiteral("source"), QStringLiteral("target"), QStringLiteral("sourcePath"), QStringLiteral("path"), QStringLiteral("currentPath"), QStringLiteral("decoyRoot"), QStringLiteral("destination"), QStringLiteral("payload"), QStringLiteral("install"), QStringLiteral("probe"), QStringLiteral("staged"), QStringLiteral("backup"), QStringLiteral("healthFile"), QStringLiteral("resultFile")};
    for (auto it = object.begin(); it != object.end(); ++it) {
      if (paths.contains(it.key()) && !pathValueAllowed(it.key(), it.value(), bareRoot && object.value(QStringLiteral("dir")).isString() && allows(object.value(QStringLiteral("dir")).toString()) && QFileInfo(object.value(QStringLiteral("dir")).toString()).isDir())) return false;
      if (it.key() == QLatin1String("paths")) {
        if (!it.value().isArray()) return false;
        for (const auto &p : it.value().toArray()) if (!pathValueAllowed(it.key(), p)) return false;
      }
      if (!valueAllowed(it.value())) return false;
    }
  }
  return true;
}
bool methodAllowed(const QString &method) {
  // Updater plans require the explicit fixture route and method-specific validation below.
  return !enabled() || method == QLatin1String("update_settings") || !method.startsWith(QLatin1String("update_")) || Updater::qaFixtureAllowed();
}
bool parametersAllowed(const QJsonObject &params, const QString &method) {
  if (enabled() && method.startsWith(QLatin1String("update_")) && method != QLatin1String("update_settings")) return Updater::qaCoreAllowed(method, params);
  return !enabled() || valueAllowed(params, method == QLatin1String("duplicate_group_members") ||
                                           method == QLatin1String("mark_duplicate_group_different"));
}
}
