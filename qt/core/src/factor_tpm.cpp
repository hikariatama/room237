// TPM 2.0 with a PIN (Linux). A random secret is sealed in a keyed-hash object under the owner hierarchy's storage key;
// the PIN is the object's authorization, so the TPM's dictionary-attack protection limits guessing. Commands run in a
// salted HMAC session with parameter encryption: neither the PIN nor the secret crosses the bus in the clear. The
// sealed blobs are bound to this TPM and stored in the vault file.
//
// tpm2-tss is loaded at run time, so the application starts without it; the factor is then unavailable.
#include "factors.h"

#include <QCryptographicHash>
#include <dlfcn.h>
#include <mutex>
#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_tctildr.h>

namespace core::factors::tpm {

namespace {
struct Api {
  decltype(&Tss2_TctiLdr_Initialize) tctiInitialize = nullptr;
  decltype(&Tss2_TctiLdr_Finalize) tctiFinalize = nullptr;
  decltype(&Esys_Initialize) initialize = nullptr;
  decltype(&Esys_Finalize) finalize = nullptr;
  decltype(&Esys_CreatePrimary) createPrimary = nullptr;
  decltype(&Esys_StartAuthSession) startAuthSession = nullptr;
  decltype(&Esys_TRSess_SetAttributes) setAttributes = nullptr;
  decltype(&Esys_Create) create = nullptr;
  decltype(&Esys_Load) load = nullptr;
  decltype(&Esys_TR_SetAuth) setAuth = nullptr;
  decltype(&Esys_Unseal) unseal = nullptr;
  decltype(&Esys_FlushContext) flush = nullptr;
  decltype(&Esys_Free) free = nullptr;
  decltype(&Tss2_MU_TPM2B_PUBLIC_Marshal) marshalPublic = nullptr;
  decltype(&Tss2_MU_TPM2B_PUBLIC_Unmarshal) unmarshalPublic = nullptr;
  decltype(&Tss2_MU_TPM2B_PRIVATE_Marshal) marshalPrivate = nullptr;
  decltype(&Tss2_MU_TPM2B_PRIVATE_Unmarshal) unmarshalPrivate = nullptr;
  QString missing;
};

template <typename T> bool resolve(void *library, const char *name, T &out) {
  out = reinterpret_cast<T>(dlsym(library, name));
  return out != nullptr;
}

const Api &api() {
  static Api loaded;
  static std::once_flag once;
  std::call_once(once, [] {
    // tpm2-tss logs every refused command, a wrong PIN included, to standard error.
    if (!qEnvironmentVariableIsSet("TSS2_LOG")) qputenv("TSS2_LOG", "all+NONE");
    void *esys = dlopen("libtss2-esys.so.0", RTLD_NOW | RTLD_LOCAL);
    void *tcti = dlopen("libtss2-tctildr.so.0", RTLD_NOW | RTLD_LOCAL);
    void *mu = dlopen("libtss2-mu.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!esys || !tcti || !mu) {
      loaded.missing = QStringLiteral("Install tpm2-tss (libtss2-esys) to use the TPM");
      return;
    }
    const bool ok = resolve(tcti, "Tss2_TctiLdr_Initialize", loaded.tctiInitialize) && resolve(tcti, "Tss2_TctiLdr_Finalize", loaded.tctiFinalize) &&
                    resolve(esys, "Esys_Initialize", loaded.initialize) && resolve(esys, "Esys_Finalize", loaded.finalize) &&
                    resolve(esys, "Esys_CreatePrimary", loaded.createPrimary) && resolve(esys, "Esys_StartAuthSession", loaded.startAuthSession) &&
                    resolve(esys, "Esys_TRSess_SetAttributes", loaded.setAttributes) && resolve(esys, "Esys_Create", loaded.create) &&
                    resolve(esys, "Esys_Load", loaded.load) && resolve(esys, "Esys_TR_SetAuth", loaded.setAuth) && resolve(esys, "Esys_Unseal", loaded.unseal) &&
                    resolve(esys, "Esys_FlushContext", loaded.flush) && resolve(esys, "Esys_Free", loaded.free) &&
                    resolve(mu, "Tss2_MU_TPM2B_PUBLIC_Marshal", loaded.marshalPublic) && resolve(mu, "Tss2_MU_TPM2B_PUBLIC_Unmarshal", loaded.unmarshalPublic) &&
                    resolve(mu, "Tss2_MU_TPM2B_PRIVATE_Marshal", loaded.marshalPrivate) && resolve(mu, "Tss2_MU_TPM2B_PRIVATE_Unmarshal", loaded.unmarshalPrivate);
    if (!ok) loaded.missing = QStringLiteral("The installed tpm2-tss is incomplete");
  });
  return loaded;
}

// The TPM's own response code, without the layer of the stack that reported it.
TPM2_RC base(TSS2_RC rc) {
  const TPM2_RC code = rc & 0xFFFF;
  return (code & TPM2_RC_FMT1) ? (code & 0xBF) | TPM2_RC_FMT1 : code;
}

[[noreturn]] void raise(TSS2_RC rc, const char *what) {
  const TPM2_RC code = base(rc);
  if (code == (TPM2_RC_AUTH_FAIL & 0xBF) || code == (TPM2_RC_BAD_AUTH & 0xBF)) fail(QStringLiteral("Wrong PIN of the TPM"));
  if (code == TPM2_RC_LOCKOUT) fail(QStringLiteral("The TPM is locked after too many wrong PINs; it unlocks after a while"));
  if (code == (TPM2_RC_INTEGRITY & 0xBF)) fail(QStringLiteral("The TPM key of this root belongs to another TPM or was reset. Use the recovery key."));
  fail(QStringLiteral("TPM: %1 failed (0x%2)").arg(QLatin1String(what)).arg(quint32(rc), 0, 16));
}
void check(TSS2_RC rc, const char *what) {
  if (rc != TSS2_RC_SUCCESS) raise(rc, what);
}

// The tpm2-tools convention: TPM2TOOLS_TCTI or TCTI picks the transport; otherwise the loader's default
// (the access broker, then /dev/tpmrm0).
QByteArray tctiConfig() {
  for (const char *name : {"TPM2TOOLS_TCTI", "TCTI"})
    if (const QByteArray value = qgetenv(name); !value.isEmpty()) return value;
  return {};
}

struct Context {
  const Api &f;
  TSS2_TCTI_CONTEXT *tcti = nullptr;
  ESYS_CONTEXT *esys = nullptr;
  ESYS_TR primary = ESYS_TR_NONE, session = ESYS_TR_NONE;
  Context() : f(api()) {
    if (!f.missing.isEmpty()) fail(f.missing);
    const QByteArray config = tctiConfig();
    if (f.tctiInitialize(config.isEmpty() ? nullptr : config.constData(), &tcti) != TSS2_RC_SUCCESS)
      fail(QStringLiteral("No TPM is available to this user; access to /dev/tpmrm0 usually comes with the tss group"));
    if (const TSS2_RC rc = f.initialize(&esys, tcti, nullptr); rc != TSS2_RC_SUCCESS) {
      f.tctiFinalize(&tcti);
      raise(rc, "initialization");
    }
  }
  ~Context() {
    if (session != ESYS_TR_NONE) f.flush(esys, session);
    if (primary != ESYS_TR_NONE) f.flush(esys, primary);
    f.finalize(&esys);
    f.tctiFinalize(&tcti);
  }
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  // The storage key of the owner hierarchy (an ECC P-256 template, the same every time), and a session salted with it
  // that encrypts the first command parameter and the first response parameter.
  void open() {
    TPM2B_SENSITIVE_CREATE sensitive = {};
    TPM2B_PUBLIC templ = {};
    templ.publicArea.type = TPM2_ALG_ECC;
    templ.publicArea.nameAlg = TPM2_ALG_SHA256;
    templ.publicArea.objectAttributes = TPMA_OBJECT_USERWITHAUTH | TPMA_OBJECT_RESTRICTED | TPMA_OBJECT_DECRYPT | TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT |
                                        TPMA_OBJECT_SENSITIVEDATAORIGIN | TPMA_OBJECT_NODA;
    templ.publicArea.parameters.eccDetail.symmetric.algorithm = TPM2_ALG_AES;
    templ.publicArea.parameters.eccDetail.symmetric.keyBits.aes = 128;
    templ.publicArea.parameters.eccDetail.symmetric.mode.aes = TPM2_ALG_CFB;
    templ.publicArea.parameters.eccDetail.scheme.scheme = TPM2_ALG_NULL;
    templ.publicArea.parameters.eccDetail.curveID = TPM2_ECC_NIST_P256;
    templ.publicArea.parameters.eccDetail.kdf.scheme = TPM2_ALG_NULL;
    TPM2B_DATA outside = {};
    TPML_PCR_SELECTION pcrs = {};
    TPM2B_PUBLIC *outPublic = nullptr;
    TPM2B_CREATION_DATA *creationData = nullptr;
    TPM2B_DIGEST *creationHash = nullptr;
    TPMT_TK_CREATION *ticket = nullptr;
    const TSS2_RC rc = f.createPrimary(esys, ESYS_TR_RH_OWNER, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE, &sensitive, &templ, &outside, &pcrs, &primary, &outPublic, &creationData,
                                       &creationHash, &ticket);
    f.free(outPublic);
    f.free(creationData);
    f.free(creationHash);
    f.free(ticket);
    if (rc != TSS2_RC_SUCCESS) {
      if (base(rc) == (TPM2_RC_BAD_AUTH & 0xBF) || base(rc) == (TPM2_RC_AUTH_FAIL & 0xBF)) fail(QStringLiteral("The TPM's owner hierarchy has a password; Room237 cannot use it"));
      raise(rc, "storage key");
    }
    TPMT_SYM_DEF symmetric = {};
    symmetric.algorithm = TPM2_ALG_AES;
    symmetric.keyBits.aes = 128;
    symmetric.mode.aes = TPM2_ALG_CFB;
    check(f.startAuthSession(esys, primary, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, nullptr, TPM2_SE_HMAC, &symmetric, TPM2_ALG_SHA256, &session), "session");
    const TPMA_SESSION flags = TPMA_SESSION_DECRYPT | TPMA_SESSION_ENCRYPT | TPMA_SESSION_CONTINUESESSION;
    check(f.setAttributes(esys, session, flags, 0xff), "session");
  }
};

TPM2B_AUTH authFrom(const QString &pin) {
  const QByteArray digest = QCryptographicHash::hash(QByteArrayLiteral("room237 tpm v1|") + pin.toUtf8(), QCryptographicHash::Sha256);
  TPM2B_AUTH auth = {};
  auth.size = UINT16(digest.size());
  memcpy(auth.buffer, digest.constData(), size_t(digest.size()));
  return auth;
}

template <typename T, typename F> QByteArray marshal(const T &value, F function) {
  QByteArray out(sizeof(T) + 16, '\0');
  size_t offset = 0;
  check(function(&value, reinterpret_cast<uint8_t *>(out.data()), size_t(out.size()), &offset), "encoding");
  out.truncate(qsizetype(offset));
  return out;
}
template <typename T, typename F> T unmarshal(const QByteArray &bytes, F function) {
  T value = {};
  size_t offset = 0;
  if (function(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()), &offset, &value) != TSS2_RC_SUCCESS || offset != size_t(bytes.size()))
    fail(QStringLiteral("Invalid TPM slot"));
  return value;
}

vault::Secret secretFrom(const QByteArray &sealed) { return vault::derive(vault::Secret(sealed), QByteArrayLiteral("room237 tpm v1")); }
}  // namespace

bool available(QString *detail) {
  try {
    Context context;
    return true;
  } catch (const Error &error) {
    if (detail) *detail = error.message();
    return false;
  }
}

Enrollment enroll(const QString &pin) {
  if (pin.isEmpty()) fail(QStringLiteral("Choose a PIN for the TPM"));
  Context context;
  context.open();
  const QByteArray data = vault::randomBytes(32);
  TPM2B_SENSITIVE_CREATE sensitive = {};
  sensitive.sensitive.userAuth = authFrom(pin);
  sensitive.sensitive.data.size = UINT16(data.size());
  memcpy(sensitive.sensitive.data.buffer, data.constData(), size_t(data.size()));
  TPM2B_PUBLIC templ = {};
  templ.publicArea.type = TPM2_ALG_KEYEDHASH;
  templ.publicArea.nameAlg = TPM2_ALG_SHA256;
  templ.publicArea.objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT | TPMA_OBJECT_USERWITHAUTH;
  templ.publicArea.parameters.keyedHashDetail.scheme.scheme = TPM2_ALG_NULL;
  TPM2B_DATA outside = {};
  TPML_PCR_SELECTION pcrs = {};
  TPM2B_PRIVATE *outPrivate = nullptr;
  TPM2B_PUBLIC *outPublic = nullptr;
  TPM2B_CREATION_DATA *creationData = nullptr;
  TPM2B_DIGEST *creationHash = nullptr;
  TPMT_TK_CREATION *ticket = nullptr;
  const TSS2_RC rc = context.f.create(context.esys, context.primary, context.session, ESYS_TR_NONE, ESYS_TR_NONE, &sensitive, &templ, &outside, &pcrs, &outPrivate, &outPublic,
                                      &creationData, &creationHash, &ticket);
  explicit_bzero(&sensitive, sizeof sensitive);
  QByteArray publicBlob, privateBlob;
  if (rc == TSS2_RC_SUCCESS) {
    publicBlob = marshal(*outPublic, context.f.marshalPublic);
    privateBlob = marshal(*outPrivate, context.f.marshalPrivate);
  }
  context.f.free(outPrivate);
  context.f.free(outPublic);
  context.f.free(creationData);
  context.f.free(creationHash);
  context.f.free(ticket);
  check(rc, "sealing");
  Enrollment out;
  out.secret = secretFrom(data);
  out.params = {{QStringLiteral("public"), QString::fromLatin1(publicBlob.toBase64())}, {QStringLiteral("private"), QString::fromLatin1(privateBlob.toBase64())}};
  // The sealed object must open with the PIN before the vault relies on it.
  if (secret(out.params, pin).bytes() != out.secret.bytes()) fail(QStringLiteral("The TPM returned another secret than it sealed"));
  return out;
}

vault::Secret secret(const QJsonObject &params, const QString &pin) {
  if (pin.isEmpty()) fail(QStringLiteral("Enter the TPM PIN"));
  Context context;
  const TPM2B_PUBLIC publicArea = unmarshal<TPM2B_PUBLIC>(QByteArray::fromBase64(params.value(QLatin1String("public")).toString().toLatin1()), context.f.unmarshalPublic);
  const TPM2B_PRIVATE privateArea = unmarshal<TPM2B_PRIVATE>(QByteArray::fromBase64(params.value(QLatin1String("private")).toString().toLatin1()), context.f.unmarshalPrivate);
  context.open();
  ESYS_TR object = ESYS_TR_NONE;
  check(context.f.load(context.esys, context.primary, context.session, ESYS_TR_NONE, ESYS_TR_NONE, &privateArea, &publicArea, &object), "loading");
  TPM2B_AUTH auth = authFrom(pin);
  TSS2_RC rc = context.f.setAuth(context.esys, object, &auth);
  explicit_bzero(&auth, sizeof auth);
  TPM2B_SENSITIVE_DATA *data = nullptr;
  if (rc == TSS2_RC_SUCCESS) rc = context.f.unseal(context.esys, object, context.session, ESYS_TR_NONE, ESYS_TR_NONE, &data);
  context.f.flush(context.esys, object);
  QByteArray sealed;
  if (rc == TSS2_RC_SUCCESS && data) sealed = QByteArray(reinterpret_cast<const char *>(data->buffer), data->size);
  if (data) explicit_bzero(data, sizeof *data);
  context.f.free(data);
  check(rc, "unsealing");
  if (sealed.size() != 32) fail(QStringLiteral("Invalid TPM slot"));
  return secretFrom(sealed);
}

}  // namespace core::factors::tpm
