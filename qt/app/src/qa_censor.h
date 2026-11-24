#pragma once
// QA builds: ROOM237_QA_CENSOR=1 turns every decoded picture into a 3x3 average stretched back to its size, so
// screenshots of a run on real media show layout, timing and colour changes but not the content.
#include <QImage>
#ifdef ROOM237_QA
inline bool qaCensorOn() {
  static const bool on = qEnvironmentVariableIntValue("ROOM237_QA_CENSOR") == 1;
  return on;
}
inline void qaCensor(QImage &image) {
  if (!qaCensorOn() || image.isNull()) return;
  const QImage::Format format = image.format();
  const QSize size = image.size();
  const QImage coarse = image.scaled(3, 3, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  image = coarse.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(format);
}
#else
inline void qaCensor(QImage &) {}
#endif
