#pragma once
// The library core inside the application process. Commands keep the method names and JSON shapes of the earlier
// separate core process, so the interface talks to it through the same call/reply contract.
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace core {

// Imports the 6.x profile once when one is given, then loads settings. Throws on corrupt settings: silently using
// defaults could turn privacy off.
void initialize(const QString &profile, const QString &legacyProfile);
// Runs one command on the calling thread. Throws core::Error with the message the interface shows.
QJsonValue dispatch(const QString &method, const QJsonObject &params);

}  // namespace core
