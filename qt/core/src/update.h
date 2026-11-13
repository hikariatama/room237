#pragma once
// Signed self-update. A release publishes qt-update.json and qt-update.json.minisig; the manifest pins version, URL,
// size and SHA-256 of one payload per platform, so one signature covers everything. The app downloads; this module
// verifies, unpacks into a sibling directory and hands the swap to room237-core, a helper without Qt that runs from a
// copy outside the install after the app exits.
#include <QJsonObject>
#include <QString>

namespace core::update {

QString currentVersion();
QJsonObject check(const QString &manifest, const QString &signature, const QString &currentVersion, const QString &platform);
QJsonObject stage(const QJsonObject &plan);
void launchHelper(const QJsonObject &plan);
void cleanup(const QString &install);

// Exposed for tests.
void verifySignature(const QString &publicKey, const QByteArray &data, const QString &signature);
bool isNewer(const QString &candidate, const QString &current);
void unpack(const QString &archive, const QString &destination);

}  // namespace core::update
