#include "secure_media.h"

#include "vault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>
#include <mutex>

namespace SecureMedia {

namespace {
std::mutex exportMutex;
QString exportDir;
}  // namespace

bool isSealed(const QString &path) { return core::vault::isEncryptedFile(path); }

QByteArray read(const QString &path, qint64 limit) {
  try {
    return core::vault::readPlain(path, limit);
  } catch (const std::exception &) {
    return {};
  }
}

std::unique_ptr<QIODevice> open(const QString &path) {
  try {
    return core::vault::openPlain(path);
  } catch (const std::exception &) {
    return nullptr;
  }
}

QString exportCopy(const QString &path, const QString &originalName, QString *error) {
  const auto keys = core::vault::keysFor(path);
  if (!keys) {
    if (error) *error = QStringLiteral("locked");
    return {};
  }
  QString dir;
  {
    std::lock_guard lock(exportMutex);
    if (exportDir.isEmpty()) {
      exportDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("room237-export-") + QUuid::createUuid().toString(QUuid::Id128));
      QDir().mkpath(exportDir);
      QFile::setPermissions(exportDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    dir = exportDir;
  }
  // One folder per copy keeps the original name even when two files share it.
  const QString folder = QDir(dir).filePath(QUuid::createUuid().toString(QUuid::Id128).left(12));
  QDir().mkpath(folder);
  const QString target = QDir(folder).filePath(QFileInfo(originalName).fileName());
  try {
    core::vault::decryptToFile(*keys, path, target);
  } catch (const std::exception &failure) {
    if (error) *error = QString::fromUtf8(failure.what());
    return {};
  }
  return target;
}

void clearExports() {
  std::lock_guard lock(exportMutex);
  if (exportDir.isEmpty()) return;
  QDir(exportDir).removeRecursively();
  exportDir.clear();
}

}  // namespace SecureMedia
