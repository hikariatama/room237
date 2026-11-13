// Security keys through libfido2. Setup makes a non-resident credential with the hmac-secret extension; S comes from
// the key's HMAC over a stored random salt, which only that key can compute, and only after a touch. The credential
// id and the salt are public and stored in the vault file.
#include "factors.h"

#include <fido.h>

#include <mutex>

namespace core::factors::fido {

namespace {
const char *const kRp = "room237";
constexpr int kTimeoutMs = 30000;

void initialize() {
  static std::once_flag once;
  std::call_once(once, [] { fido_init(0); });
}

[[noreturn]] void raise(int code) {
  switch (code) {
    case FIDO_ERR_PIN_REQUIRED:
    case FIDO_ERR_UV_INVALID:
      fail(QStringLiteral("This security key needs its PIN"));
    case FIDO_ERR_PIN_INVALID:
    case FIDO_ERR_PIN_AUTH_INVALID:
      fail(QStringLiteral("Wrong PIN of the security key"));
    case FIDO_ERR_PIN_BLOCKED:
    case FIDO_ERR_PIN_AUTH_BLOCKED:
    case FIDO_ERR_UV_BLOCKED:
      fail(QStringLiteral("The PIN of the security key is blocked"));
    case FIDO_ERR_ACTION_TIMEOUT:
    case FIDO_ERR_USER_ACTION_TIMEOUT:
    case FIDO_ERR_RX:
      fail(QStringLiteral("The security key was not touched in time"));
    case FIDO_ERR_OPERATION_DENIED:
    case FIDO_ERR_KEEPALIVE_CANCEL:
      fail(QStringLiteral("Confirmation cancelled"));
    case FIDO_ERR_UNSUPPORTED_EXTENSION:
    case FIDO_ERR_UNSUPPORTED_OPTION:
      fail(QStringLiteral("This security key does not support hmac-secret"));
    case FIDO_ERR_NO_CREDENTIALS:
      fail(QStringLiteral("This security key does not hold the key of this root"));
    default:
      fail(QStringLiteral("Security key: %1").arg(QString::fromUtf8(fido_strerr(code))));
  }
}
void check(int code) {
  if (code != FIDO_OK) raise(code);
}

struct Device {
  fido_dev_t *dev = nullptr;
  explicit Device(const char *path) {
    dev = fido_dev_new();
    if (!dev) fail(QStringLiteral("Out of memory"));
    if (const int r = fido_dev_open(dev, path); r != FIDO_OK) {
      fido_dev_free(&dev);
      raise(r);
    }
    fido_dev_set_timeout(dev, kTimeoutMs);
  }
  ~Device() {
    if (!dev) return;
    fido_dev_close(dev);
    fido_dev_free(&dev);
  }
  Device(const Device &) = delete;
  Device &operator=(const Device &) = delete;
};

// Paths of the connected FIDO2 devices.
QList<QByteArray> devices() {
  initialize();
  constexpr size_t kMax = 16;
  fido_dev_info_t *list = fido_dev_info_new(kMax);
  if (!list) fail(QStringLiteral("Out of memory"));
  size_t found = 0;
  QList<QByteArray> out;
  if (fido_dev_info_manifest(list, kMax, &found) == FIDO_OK)
    for (size_t i = 0; i < found; ++i) out << QByteArray(fido_dev_info_path(fido_dev_info_ptr(list, i)));
  fido_dev_info_free(&list, kMax);
  return out;
}

const char *pinOrNull(const QByteArray &pin) { return pin.isEmpty() ? nullptr : pin.constData(); }

// The key's hmac-secret over `salt` for the credential `id`.
QByteArray hmacSecret(fido_dev_t *dev, const QByteArray &id, const QByteArray &salt, const QByteArray &pin) {
  fido_assert_t *assertion = fido_assert_new();
  if (!assertion) fail(QStringLiteral("Out of memory"));
  const QByteArray clientData = vault::randomBytes(32);
  int r = fido_assert_set_clientdata_hash(assertion, reinterpret_cast<const unsigned char *>(clientData.constData()), size_t(clientData.size()));
  if (r == FIDO_OK) r = fido_assert_set_rp(assertion, kRp);
  if (r == FIDO_OK) r = fido_assert_allow_cred(assertion, reinterpret_cast<const unsigned char *>(id.constData()), size_t(id.size()));
  if (r == FIDO_OK) r = fido_assert_set_extensions(assertion, FIDO_EXT_HMAC_SECRET);
  if (r == FIDO_OK) r = fido_assert_set_hmac_salt(assertion, reinterpret_cast<const unsigned char *>(salt.constData()), size_t(salt.size()));
  if (r == FIDO_OK) r = fido_assert_set_up(assertion, FIDO_OPT_TRUE);
  if (r == FIDO_OK) r = fido_dev_get_assert(dev, assertion, pinOrNull(pin));
  QByteArray out;
  if (r == FIDO_OK && fido_assert_count(assertion) == 1 && fido_assert_hmac_secret_len(assertion, 0) >= 32)
    out = QByteArray(reinterpret_cast<const char *>(fido_assert_hmac_secret_ptr(assertion, 0)), qsizetype(fido_assert_hmac_secret_len(assertion, 0)));
  fido_assert_free(&assertion);
  check(r);
  if (out.isEmpty()) fail(QStringLiteral("This security key does not support hmac-secret"));
  return out;
}

vault::Secret secretFrom(const QByteArray &hmac) { return vault::derive(vault::Secret(hmac), QByteArrayLiteral("room237 fido v1")); }
}  // namespace

bool available(QString *) { return true; }

Enrollment enroll(const QString &pinText) {
  const QList<QByteArray> paths = devices();
  if (paths.isEmpty()) fail(QStringLiteral("Connect a security key"));
  const QByteArray pin = pinText.toUtf8();
  Device device(paths.first().constData());
  if (!fido_dev_is_fido2(device.dev)) fail(QStringLiteral("This security key does not support hmac-secret"));
  fido_cred_t *cred = fido_cred_new();
  if (!cred) fail(QStringLiteral("Out of memory"));
  const QByteArray clientData = vault::randomBytes(32), user = vault::randomBytes(16);
  int r = fido_cred_set_type(cred, COSE_ES256);
  if (r == FIDO_OK) r = fido_cred_set_clientdata_hash(cred, reinterpret_cast<const unsigned char *>(clientData.constData()), size_t(clientData.size()));
  if (r == FIDO_OK) r = fido_cred_set_rp(cred, kRp, "Room237");
  if (r == FIDO_OK) r = fido_cred_set_user(cred, reinterpret_cast<const unsigned char *>(user.constData()), size_t(user.size()), "room237", "Room237", nullptr);
  if (r == FIDO_OK) r = fido_cred_set_extensions(cred, FIDO_EXT_HMAC_SECRET);
  if (r == FIDO_OK) r = fido_cred_set_rk(cred, FIDO_OPT_FALSE);
  if (r == FIDO_OK) r = fido_dev_make_cred(device.dev, cred, pinOrNull(pin));
  QByteArray id;
  if (r == FIDO_OK) id = QByteArray(reinterpret_cast<const char *>(fido_cred_id_ptr(cred)), qsizetype(fido_cred_id_len(cred)));
  fido_cred_free(&cred);
  check(r);
  if (id.isEmpty()) fail(QStringLiteral("The security key returned no credential"));
  // The output with and without the PIN differs, so the unlock must use it the same way.
  const QByteArray salt = vault::randomBytes(32);
  Enrollment out;
  out.secret = secretFrom(hmacSecret(device.dev, id, salt, pin));
  out.params = {{QStringLiteral("credential"), QString::fromLatin1(id.toBase64())}, {QStringLiteral("salt"), QString::fromLatin1(salt.toBase64())}, {QStringLiteral("pin"), !pin.isEmpty()}};
  return out;
}

vault::Secret secret(const QJsonObject &params, const QString &pinText) {
  const QByteArray id = QByteArray::fromBase64(params.value(QLatin1String("credential")).toString().toLatin1());
  const QByteArray salt = QByteArray::fromBase64(params.value(QLatin1String("salt")).toString().toLatin1());
  if (id.isEmpty() || salt.size() != 32) fail(QStringLiteral("Invalid security key slot"));
  const QByteArray pin = pinText.toUtf8();
  if (params.value(QLatin1String("pin")).toBool() && pin.isEmpty()) fail(QStringLiteral("This security key needs its PIN"));
  const QList<QByteArray> paths = devices();
  if (paths.isEmpty()) fail(QStringLiteral("Connect a security key"));
  // Another connected key answers that it holds no such credential; the next one is asked.
  int lastError = FIDO_ERR_NO_CREDENTIALS;
  for (const QByteArray &path : paths) {
    try {
      Device device(path.constData());
      return secretFrom(hmacSecret(device.dev, id, salt, params.value(QLatin1String("pin")).toBool() ? pin : QByteArray()));
    } catch (const Error &error) {
      if (!error.message().contains(QLatin1String("does not hold the key"))) throw;
      lastError = FIDO_ERR_NO_CREDENTIALS;
    }
  }
  raise(lastError);
}

}  // namespace core::factors::fido
