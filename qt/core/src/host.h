#pragma once
// Events from the core to the interface. The sink runs on the emitting thread and must be thread-safe.
#include <QJsonValue>
#include <QString>
#include <functional>

namespace core::host {

using EventSink = std::function<void(const QString &name, const QJsonValue &payload)>;
void setEventSink(EventSink sink);
void emitEvent(const QString &name, const QJsonValue &payload);

}  // namespace core::host
