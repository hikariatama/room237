#include "factors.h"

#include "util.h"

namespace core::factors {

namespace {
#if defined(ROOM237_QA) || defined(ROOM237_TEST_FACTOR)
// A factor without a prompt for automated tests: S comes from the stored seed. ROOM237_QA_FACTOR=cancel makes it refuse.
bool testFactorEnabled() {
#ifdef ROOM237_TEST_FACTOR
  return true;
#else
  return qaRun();
#endif
}
Enrollment testEnroll() {
  Enrollment out;
  const QByteArray seed = vault::randomBytes(32);
  out.params = {{QStringLiteral("seed"), QString::fromLatin1(seed.toBase64())}};
  out.secret = vault::derive(vault::Secret(seed), QByteArrayLiteral("room237 test factor"));
  return out;
}
vault::Secret testSecret(const QJsonObject &params) {
  if (qEnvironmentVariable("ROOM237_QA_FACTOR") == QLatin1String("cancel")) fail(QStringLiteral("Confirmation cancelled"));
  return vault::derive(vault::Secret(QByteArray::fromBase64(params.value(QLatin1String("seed")).toString().toLatin1())), QByteArrayLiteral("room237 test factor"));
}
#else
bool testFactorEnabled() { return false; }
Enrollment testEnroll() { fail(QStringLiteral("This second factor is not available on this device")); }
vault::Secret testSecret(const QJsonObject &) { fail(QStringLiteral("This second factor is not available on this device")); }
#endif
const QString kTestKind = QStringLiteral("test");
}  // namespace

std::vector<Availability> available() {
  std::vector<Availability> out;
  QString detail;
#if defined(Q_OS_MACOS)
  out.push_back({QStringLiteral("macos-se"), mac::available(&detail), true, detail});
#elif defined(Q_OS_WIN)
  out.push_back({QStringLiteral("windows-hello"), windows::available(&detail), true, detail});
#else
  out.push_back({QStringLiteral("tpm2"), tpm::available(&detail), true, detail});
#endif
  detail.clear();
  out.push_back({QStringLiteral("fido2"), fido::available(&detail), false, detail});
  if (testFactorEnabled()) out.push_back({kTestKind, true, true, {}});
  return out;
}

Enrollment enroll(const QString &kind, const QString &reason, const QString &pin) {
  if (kind == kTestKind && testFactorEnabled()) return testEnroll();
#if defined(Q_OS_MACOS)
  if (kind == QLatin1String("macos-se")) return mac::enroll(reason);
#elif defined(Q_OS_WIN)
  if (kind == QLatin1String("windows-hello")) return windows::enroll(reason);
#else
  if (kind == QLatin1String("tpm2")) return tpm::enroll(pin);
#endif
  if (kind == QLatin1String("fido2")) return fido::enroll(pin);
  Q_UNUSED(reason)
  fail(QStringLiteral("This second factor is not available on this device"));
}

vault::Secret secret(const QString &kind, const QJsonObject &params, const QString &reason, const QString &pin) {
  if (kind == kTestKind && testFactorEnabled()) return testSecret(params);
#if defined(Q_OS_MACOS)
  if (kind == QLatin1String("macos-se")) return mac::secret(params, reason);
#elif defined(Q_OS_WIN)
  if (kind == QLatin1String("windows-hello")) return windows::secret(params, reason);
#else
  if (kind == QLatin1String("tpm2")) return tpm::secret(params, pin);
#endif
  if (kind == QLatin1String("fido2")) return fido::secret(params, pin);
  Q_UNUSED(reason)
  fail(QStringLiteral("This second factor is not available on this device"));
}

void forget(const QString &kind, const QJsonObject &params) {
#if defined(Q_OS_WIN)
  if (kind == QLatin1String("windows-hello")) windows::forget(params);
#else
  Q_UNUSED(kind) Q_UNUSED(params)
#endif
}

}  // namespace core::factors
