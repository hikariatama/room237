#include "clipboard.h"

#include "fs.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMimeData>
#include <QThread>
#include <QUrl>

namespace core::clipboard {

void setClipboardFiles(const QStringList &paths) {
  if (paths.isEmpty()) fail(QStringLiteral("No paths provided"));
  QList<QUrl> urls;
  for (const QString &p : paths) {
    if (!path::isAbsolute(p)) fail(QStringLiteral("Path is not absolute: %1").arg(p));
    const FileInfo info = stat(p);
    if (!info.exists) fail(QStringLiteral("Path does not exist: %1").arg(p));
    if (!info.isFile) fail(QStringLiteral("Path is not a regular file: %1").arg(p));
    urls << QUrl::fromLocalFile(p);
  }
  auto publish = [urls] {
    auto *data = new QMimeData;
    data->setUrls(urls);
#if defined(Q_OS_LINUX)
    QByteArray gnome("copy");
    for (const QUrl &url : urls) gnome += '\n' + url.toEncoded();
    data->setData(QStringLiteral("x-special/gnome-copied-files"), gnome + '\n');
#elif defined(Q_OS_WIN)
    const quint32 copy = 1;  // DROPEFFECT_COPY
    data->setData(QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\""), QByteArray(reinterpret_cast<const char *>(&copy), sizeof copy));
#endif
    QGuiApplication::clipboard()->setMimeData(data);
  };
  // QClipboard belongs to the interface thread.
  auto *app = QCoreApplication::instance();
  if (!app) fail(QStringLiteral("Clipboard is unavailable"));
  if (QThread::currentThread() == app->thread()) publish();
  else QMetaObject::invokeMethod(app, publish, Qt::BlockingQueuedConnection);
}

}  // namespace core::clipboard
