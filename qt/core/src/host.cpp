#include "host.h"

#include <mutex>

namespace core::host {

namespace {
std::mutex sinkMutex;
EventSink currentSink;
}  // namespace

void setEventSink(EventSink sink) {
  std::lock_guard lock(sinkMutex);
  currentSink = std::move(sink);
}

void emitEvent(const QString &name, const QJsonValue &payload) {
  EventSink sink;
  {
    std::lock_guard lock(sinkMutex);
    sink = currentSink;
  }
  if (sink) sink(name, payload);
}

}  // namespace core::host
