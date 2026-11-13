#pragma once
// Reading media of encrypted roots. Files there are sealed by the core; decoders get their plaintext from memory or
// a decrypting device, never from a plain copy on disk. Copies leave the room only through exportCopy().
#include <QIODevice>
#include <QString>
#include <memory>

namespace SecureMedia {
bool isSealed(const QString &path);
// The whole plaintext; empty when the file cannot be decrypted (the root is locked or the file is damaged).
QByteArray read(const QString &path, qint64 limit = qint64(1) << 30);
// Random-access plaintext for the video player; nullptr when the file cannot be decrypted.
std::unique_ptr<QIODevice> open(const QString &path);
// A decrypted copy under its original name in this run's export folder, for the clipboard, a drag or another app.
// Empty with `error` set on failure.
QString exportCopy(const QString &path, const QString &originalName, QString *error = nullptr);
// Removes every decrypted copy; called when the room locks and when the app quits.
void clearExports();
}  // namespace SecureMedia
