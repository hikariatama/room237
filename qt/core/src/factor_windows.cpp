// Windows Hello: a key credential of KeyCredentialManager, an RSA key kept by the device's TPM (or its software
// fallback) for this Windows account, used after Windows Hello confirms the user. Its PKCS #1 v1.5 signatures are
// deterministic, so S comes from the signature over a stored random challenge.
#include "factors.h"

// mingw-w64 specializes IReference for BYTE and for boolean, which are the same type; IReference<boolean> is not used.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#include <roapi.h>
#include <windows.h>
#include <windows.foundation.h>
#include <windows.security.credentials.h>
#include <windows.security.cryptography.h>
#include <windows.storage.streams.h>
#include <winstring.h>

#include <chrono>
#include <thread>

using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Security::Credentials;
using ABI::Windows::Security::Cryptography::ICryptographicBufferStatics;
using ABI::Windows::Storage::Streams::IBuffer;

namespace core::factors::windows {

namespace {
constexpr auto kTimeout = std::chrono::minutes(3);

template <typename T> struct Com {
  T *p = nullptr;
  Com() = default;
  Com(const Com &) = delete;
  Com &operator=(const Com &) = delete;
  ~Com() {
    if (p) p->Release();
  }
  T **put() { return &p; }
  T *operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

struct HString {
  HSTRING h = nullptr;
  explicit HString(const QString &text) { WindowsCreateString(reinterpret_cast<const wchar_t *>(text.utf16()), UINT32(text.size()), &h); }
  ~HString() { WindowsDeleteString(h); }
  HString(const HString &) = delete;
  HString &operator=(const HString &) = delete;
};

// WinRT on this thread, as a multi-threaded apartment unless the thread already chose one.
struct Apartment {
  bool owned = false;
  Apartment() {
    const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    owned = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) fail(QStringLiteral("Windows Runtime is unavailable (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0')));
  }
  ~Apartment() {
    if (owned) RoUninitialize();
  }
};

void check(HRESULT hr, const char *what) {
  if (FAILED(hr)) fail(QStringLiteral("Windows Hello: %1 failed (0x%2)").arg(QLatin1String(what)).arg(quint32(hr), 8, 16, QLatin1Char('0')));
}

template <typename T> void factory(const wchar_t *name, T **out) {
  HSTRING_HEADER header;
  HSTRING id = nullptr;
  check(WindowsCreateStringReference(name, UINT32(wcslen(name)), &header, &id), "class lookup");
  check(RoGetActivationFactory(id, __uuidof(T), reinterpret_cast<void **>(out)), "activation");
}

// The system dialog of Windows Hello opens behind a desktop application's window; bring it forward once it exists.
void raisePrompt(bool &raised) {
  if (raised) return;
  if (HWND prompt = FindWindowW(L"Credential Dialog Xaml Host", nullptr)) {
    SetForegroundWindow(prompt);
    raised = true;
  }
}

// Waits for an asynchronous operation and returns its results.
template <typename Operation, typename Result> void await(Operation *operation, Result *out, const char *what) {
  Com<IAsyncInfo> info;
  check(operation->QueryInterface(__uuidof(IAsyncInfo), reinterpret_cast<void **>(info.put())), what);
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  bool raised = false;
  AsyncStatus status = Started;
  while (SUCCEEDED(info->get_Status(&status)) && status == Started) {
    if (std::chrono::steady_clock::now() > deadline) {
      info->Cancel();
      fail(QStringLiteral("Confirmation cancelled"));
    }
    raisePrompt(raised);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (status == Canceled) fail(QStringLiteral("Confirmation cancelled"));
  if (status != Completed) {
    HRESULT error = E_FAIL;
    info->get_ErrorCode(&error);
    check(error, what);
  }
  check(operation->GetResults(out), what);
}

void checkStatus(KeyCredentialStatus status) {
  switch (status) {
    case KeyCredentialStatus_Success:
      return;
    case KeyCredentialStatus_UserCanceled:
    case KeyCredentialStatus_UserPrefersPassword:
      fail(QStringLiteral("Confirmation cancelled"));
    case KeyCredentialStatus_NotFound:
      fail(QStringLiteral("The Windows Hello key of this root no longer exists on this computer. Use the recovery key."));
    case KeyCredentialStatus_SecurityDeviceLocked:
      fail(QStringLiteral("Windows Hello is locked after too many attempts. Unlock Windows with the PIN and try again."));
    default:
      fail(QStringLiteral("Windows Hello refused the operation"));
  }
}

QByteArray sign(IKeyCredential *credential, const QByteArray &challenge) {
  Com<ICryptographicBufferStatics> buffers;
  factory(L"Windows.Security.Cryptography.CryptographicBuffer", buffers.put());
  Com<IBuffer> data;
  check(buffers->CreateFromByteArray(UINT32(challenge.size()), reinterpret_cast<BYTE *>(const_cast<char *>(challenge.constData())), data.put()), "buffer");
  Com<IAsyncOperation<KeyCredentialOperationResult *>> operation;
  check(credential->RequestSignAsync(data.p, operation.put()), "signing");
  Com<IKeyCredentialOperationResult> result;
  await(operation.p, result.put(), "signing");
  KeyCredentialStatus status = KeyCredentialStatus_UnknownError;
  check(result->get_Status(&status), "signing");
  checkStatus(status);
  Com<IBuffer> signature;
  check(result->get_Result(signature.put()), "signing");
  UINT32 length = 0;
  BYTE *bytes = nullptr;
  check(buffers->CopyToByteArray(signature.p, &length, &bytes), "signature");
  QByteArray out(reinterpret_cast<const char *>(bytes), qsizetype(length));
  CoTaskMemFree(bytes);
  if (out.isEmpty()) fail(QStringLiteral("Windows Hello returned no signature"));
  return out;
}

// Opens (or creates) the named credential.
void credential(const QString &name, bool create, IKeyCredential **out) {
  Com<IKeyCredentialManagerStatics> manager;
  factory(L"Windows.Security.Credentials.KeyCredentialManager", manager.put());
  HString id(name);
  Com<IAsyncOperation<KeyCredentialRetrievalResult *>> operation;
  if (create) check(manager->RequestCreateAsync(id.h, KeyCredentialCreationOption_ReplaceExisting, operation.put()), "key creation");
  else check(manager->OpenAsync(id.h, operation.put()), "key lookup");
  Com<IKeyCredentialRetrievalResult> result;
  await(operation.p, result.put(), create ? "key creation" : "key lookup");
  KeyCredentialStatus status = KeyCredentialStatus_UnknownError;
  check(result->get_Status(&status), "key lookup");
  checkStatus(status);
  check(result->get_Credential(out), "key lookup");
}

vault::Secret secretFrom(const QByteArray &signature) { return vault::derive(vault::Secret(signature), QByteArrayLiteral("room237 windows hello v1")); }
}  // namespace

bool available(QString *detail) {
  try {
    Apartment apartment;
    Com<IKeyCredentialManagerStatics> manager;
    factory(L"Windows.Security.Credentials.KeyCredentialManager", manager.put());
    Com<IAsyncOperation<boolean>> operation;
    check(manager->IsSupportedAsync(operation.put()), "support check");
    boolean supported = 0;
    await(operation.p, &supported, "support check");
    if (!supported && detail) *detail = QStringLiteral("Windows Hello is not set up on this computer");
    return supported;
  } catch (const Error &error) {
    if (detail) *detail = error.message();
    return false;
  }
}

Enrollment enroll(const QString &) {
  Apartment apartment;
  const QString name = QStringLiteral("room237-") + toHex(vault::randomBytes(8));
  const QByteArray challenge = vault::randomBytes(32);
  Com<IKeyCredential> key;
  credential(name, true, key.put());
  Enrollment out;
  out.secret = secretFrom(sign(key.p, challenge));
  out.params = {{QStringLiteral("name"), name}, {QStringLiteral("challenge"), QString::fromLatin1(challenge.toBase64())}};
  return out;
}

vault::Secret secret(const QJsonObject &params, const QString &) {
  const QString name = params.value(QLatin1String("name")).toString();
  const QByteArray challenge = QByteArray::fromBase64(params.value(QLatin1String("challenge")).toString().toLatin1());
  if (name.isEmpty() || challenge.size() != 32) fail(QStringLiteral("Invalid Windows Hello slot"));
  Apartment apartment;
  Com<IKeyCredential> key;
  credential(name, false, key.put());
  return secretFrom(sign(key.p, challenge));
}

void forget(const QJsonObject &params) {
  const QString name = params.value(QLatin1String("name")).toString();
  if (name.isEmpty()) return;
  try {
    Apartment apartment;
    Com<IKeyCredentialManagerStatics> manager;
    factory(L"Windows.Security.Credentials.KeyCredentialManager", manager.put());
    HString id(name);
    Com<IAsyncAction> operation;
    if (FAILED(manager->DeleteAsync(id.h, operation.put()))) return;
    Com<IAsyncInfo> info;
    if (FAILED(operation->QueryInterface(__uuidof(IAsyncInfo), reinterpret_cast<void **>(info.put())))) return;
    AsyncStatus status = Started;
    for (int i = 0; i < 500 && SUCCEEDED(info->get_Status(&status)) && status == Started; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } catch (const Error &) {
  }
}

}  // namespace core::factors::windows
