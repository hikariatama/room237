#pragma once
// Diagnostic timeline, written only when ROOM237_TRACE=<file> is set.
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
void qaTraceInit();
QStringList qaLogLines();  // The latest application messages, for the logger panel.
extern bool gQaTraceOn;
void qaTraceWrite(const char *kind, const QString &detail);
inline void qaTrace(const char *kind, const QString &detail = {}) {
  if (gQaTraceOn) qaTraceWrite(kind, detail);
}
class QaSpan {
public:
  QaSpan(const char *kind, QString detail = {}) : kind_(kind), detail_(std::move(detail)) {
    if (gQaTraceOn) clock_.start();
  }
  ~QaSpan() {
    if (gQaTraceOn) qaTraceWrite(kind_, QStringLiteral("%1 ms=%2").arg(detail_).arg(clock_.nsecsElapsed() / 1e6, 0, 'f', 2));
  }
private:
  const char *kind_;
  QString detail_;
  QElapsedTimer clock_;
};
