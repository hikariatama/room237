#include "factors.h"

// Factors without an implementation on this platform report themselves unavailable.
namespace core::factors {

namespace {
[[noreturn]] void unavailable() { fail(QStringLiteral("This second factor is not available on this device")); }
}  // namespace

#ifndef Q_OS_WIN
namespace windows {
bool available(QString *detail) {
  if (detail) *detail = QStringLiteral("Windows Hello is available only on Windows");
  return false;
}
Enrollment enroll(const QString &) { unavailable(); }
vault::Secret secret(const QJsonObject &, const QString &) { unavailable(); }
void forget(const QJsonObject &) {}
}  // namespace windows
#endif

#ifndef ROOM237_TPM
namespace tpm {
bool available(QString *detail) {
  if (detail) {
#if defined(Q_OS_LINUX)
    *detail = QStringLiteral("This build has no TPM support");
#else
    *detail = QStringLiteral("TPM 2.0 with a PIN is used on Linux");
#endif
  }
  return false;
}
Enrollment enroll(const QString &) { unavailable(); }
vault::Secret secret(const QJsonObject &, const QString &) { unavailable(); }
}  // namespace tpm
#endif

#ifndef Q_OS_MACOS
namespace mac {
bool available(QString *detail) {
  if (detail) *detail = QStringLiteral("The Secure Enclave is available only on a Mac");
  return false;
}
Enrollment enroll(const QString &) { unavailable(); }
vault::Secret secret(const QJsonObject &, const QString &) { unavailable(); }
}  // namespace mac
#endif

#ifndef ROOM237_FIDO
namespace fido {
bool available(QString *detail) {
  if (detail) *detail = QStringLiteral("Security keys are not supported in this version");
  return false;
}
Enrollment enroll(const QString &) { unavailable(); }
vault::Secret secret(const QJsonObject &, const QString &) { unavailable(); }
}  // namespace fido
#endif

}  // namespace core::factors
