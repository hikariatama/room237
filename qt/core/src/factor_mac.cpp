#include "factors.h"

#ifdef Q_OS_MACOS
extern "C" {
int room237_se_available();
int room237_se_create(const char *reason, unsigned char *blob, int blobCapacity, int *blobLength, unsigned char *peer, unsigned char *secret, char *error, int errorCapacity);
int room237_se_agree(const unsigned char *blob, int blobLength, const unsigned char *peer, const char *reason, unsigned char *secret, char *error, int errorCapacity);
}

namespace core::factors::mac {

namespace {
[[noreturn]] void raise(const char *error) {
  const QString text = QString::fromUtf8(error);
  if (text == QLatin1String("cancelled")) fail(QStringLiteral("Confirmation cancelled"));
  if (text == QLatin1String("invalidated")) fail(QStringLiteral("The Secure Enclave key no longer works: the fingerprints changed. Use the recovery key."));
  fail(text.isEmpty() ? QStringLiteral("Secure Enclave refused the operation") : text);
}
}  // namespace

bool available(QString *detail) {
  const bool ok = room237_se_available() != 0;
  if (!ok && detail) *detail = QStringLiteral("This Mac has no Secure Enclave");
  return ok;
}

Enrollment enroll(const QString &reason) {
  QByteArray blob(4096, '\0'), peer(65, '\0'), secret(32, '\0');
  int length = 0;
  char error[512] = {};
  if (!room237_se_create(reason.toUtf8().constData(), reinterpret_cast<unsigned char *>(blob.data()), int(blob.size()), &length, reinterpret_cast<unsigned char *>(peer.data()), reinterpret_cast<unsigned char *>(secret.data()), error, int(sizeof error)))
    raise(error);
  blob.truncate(length);
  Enrollment out;
  out.params = {{QStringLiteral("blob"), QString::fromLatin1(blob.toBase64())}, {QStringLiteral("peer"), QString::fromLatin1(peer.toBase64())}};
  out.secret = vault::Secret(secret);
  return out;
}

vault::Secret secret(const QJsonObject &params, const QString &reason) {
  const QByteArray blob = QByteArray::fromBase64(params.value(QLatin1String("blob")).toString().toLatin1());
  const QByteArray peer = QByteArray::fromBase64(params.value(QLatin1String("peer")).toString().toLatin1());
  if (blob.isEmpty() || peer.size() != 65) fail(QStringLiteral("Invalid Secure Enclave slot"));
  QByteArray out(32, '\0');
  char error[512] = {};
  if (!room237_se_agree(reinterpret_cast<const unsigned char *>(blob.constData()), int(blob.size()), reinterpret_cast<const unsigned char *>(peer.constData()), reason.toUtf8().constData(), reinterpret_cast<unsigned char *>(out.data()), error, int(sizeof error)))
    raise(error);
  return vault::Secret(out);
}

}  // namespace core::factors::mac
#endif
