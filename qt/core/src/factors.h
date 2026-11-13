#pragma once
// Second factors. Each contributes a secret S that only this device (Secure Enclave, Windows Hello, a TPM) or a
// security key (FIDO2 hmac-secret) can reproduce, after the user confirms presence. The password alone opens nothing.
#include "vault.h"

#include <QJsonObject>
#include <QStringList>

namespace core::factors {

// "macos-se": Secure Enclave P-256 key, Touch ID or the Mac's password (CryptoKit, .biometryCurrentSet.or.devicePasscode)
// "windows-hello": a Windows Hello key credential; S = SHA-256 of its signature over a stored challenge
// "tpm2": a secret sealed in the TPM, released with a PIN
// "fido2": a security key's hmac-secret over a stored salt
struct Availability {
  QString kind;
  bool available = false;
  bool system = false;  // The platform's own prompt, as opposed to a separate device.
  QString detail;       // Why it is unavailable, for the setup screen.
};
std::vector<Availability> available();

struct Enrollment {
  QJsonObject params;
  vault::Secret secret;
};
// Creates the factor and returns S. `reason` is shown by the system prompt; `pin` is for tpm2 and PIN-protected keys.
Enrollment enroll(const QString &kind, const QString &reason, const QString &pin);
vault::Secret secret(const QString &kind, const QJsonObject &params, const QString &reason, const QString &pin);
// Removes what enroll created on this device (the Windows Hello credential); harmless when there is nothing.
void forget(const QString &kind, const QJsonObject &params);

// Platform parts.
namespace mac {
bool available(QString *detail);
Enrollment enroll(const QString &reason);
vault::Secret secret(const QJsonObject &params, const QString &reason);
}  // namespace mac
namespace windows {
bool available(QString *detail);
Enrollment enroll(const QString &reason);
vault::Secret secret(const QJsonObject &params, const QString &reason);
void forget(const QJsonObject &params);
}  // namespace windows
namespace tpm {
bool available(QString *detail);
Enrollment enroll(const QString &pin);
vault::Secret secret(const QJsonObject &params, const QString &pin);
}  // namespace tpm
namespace fido {
bool available(QString *detail);
Enrollment enroll(const QString &pin);
vault::Secret secret(const QJsonObject &params, const QString &pin);
}  // namespace fido

}  // namespace core::factors
