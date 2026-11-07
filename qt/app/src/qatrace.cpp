#include "qatrace.h"
#include <QTime>
#include <cstdio>
#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QTextStream>
bool gQaTraceOn = false;
static QFile *gFile = nullptr;
static QMutex gMutex;
static QStringList gLines;
static void captureMessage(QtMsgType type, const QMessageLogContext &, const QString &message) {
  {
    QMutexLocker lock(&gMutex);
    gLines.push_back(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + (type >= QtCriticalMsg ? QStringLiteral(" error ") : QStringLiteral(" ")) + message);
    if (gLines.size() > 200) gLines.removeFirst();
  }
  if (gQaTraceOn) qaTraceWrite("log", message);
  else fprintf(stderr, "%s\n", qPrintable(message));
}
QStringList qaLogLines() {
  QMutexLocker lock(&gMutex);
  return gLines;
}
void qaTraceInit() {
  qInstallMessageHandler(captureMessage);
  const QByteArray path = qgetenv("ROOM237_TRACE");
  if (path.isEmpty()) return;
  gFile = new QFile(QString::fromLocal8Bit(path));
  gQaTraceOn = gFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered);
  // A Windows GUI process has no stderr: warnings go to the trace as well.
}
void qaTraceWrite(const char *kind, const QString &detail) {
  const QByteArray line = QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + ' ' + kind + ' ' + detail.toUtf8() + '\n';
  QMutexLocker lock(&gMutex);
  gFile->write(line);
}
