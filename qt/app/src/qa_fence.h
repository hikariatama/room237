#pragma once
#include <QString>
#include <QJsonObject>
// The fence confines a QA run (ROOM237_QA_PORT/ROOM237_QA_FENCE) to a disposable directory. It is compiled only into
// builds configured with ROOM237_QA; in every other build enabled() is a constant false and each check passes.
namespace QaFence {
#ifdef ROOM237_QA
bool enabled();
// Canonical containment within an explicit absolute directory. No basename/component fallback.
// Roots must exist; output/profile leaves may be missing.
bool inside(const QString &path, const QString &fence, bool allowMissing = false);
bool allows(const QString &path, bool allowMissing = false);
bool rootsAllowed(const QJsonObject &state);
bool profileAllowed(const QString &profile, bool legacy = false);
bool methodAllowed(const QString &method);
bool parametersAllowed(const QJsonObject &params, const QString &method = {});
#else
constexpr bool enabled() { return false; }
inline bool allows(const QString &, bool = false) { return true; }
inline bool rootsAllowed(const QJsonObject &) { return true; }
inline bool methodAllowed(const QString &) { return true; }
inline bool parametersAllowed(const QJsonObject &, const QString & = {}) { return true; }
#endif
}
