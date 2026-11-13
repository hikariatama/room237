#pragma once
// Commands of security hardening: set up, unlock, lock, encrypt and decrypt a root.
#include <QJsonObject>
#include <QString>

namespace core::vault_ops {

// {vault, locked, state, factors:[kinds], pinFactors:[kinds unlocked with a PIN], resumable}; {vault: false, nested} outside a vault or inside one below its root
QJsonObject status(const QString &root);
// Kinds this device can use, for the setup screen: [{kind, available, system, detail}]
QJsonObject factors();
// Creates the vault of `root`: the password, the chosen second factors, a recovery key and an optional decoy password.
// Unlocks the root and starts the encryption pass. Returns {recoveryKey}.
QJsonObject setup(const QString &root, const QString &password, const QStringList &factorKinds, const QString &decoyPassword, const QString &reason, const QString &pin);
// Password and a second factor (or the recovery key). Returns {decoy: true} when the password is the decoy password.
QJsonObject unlock(const QString &root, const QString &password, const QString &kind, const QString &reason, const QString &pin, const QString &recoveryKey);
// Second factor only, with the password kept in memory since the first unlock of this run.
QJsonObject resume(const QString &root, const QString &kind, const QString &reason, const QString &pin);
void lock(const QString &root);
void lockAll();
// Encrypts every plain media file of the root; resumes after a crash. Runs in the background and reports
// "vault-progress" events. Files that appear later are encrypted by the next pass.
void encrypt(const QString &root);
// Restores every file of the root to its original name and content and removes the vault.
void decrypt(const QString &root);
// A new second factor for this device after a fingerprint change or on another computer, unlocked with the recovery key.
void addFactor(const QString &root, const QString &kind, const QString &reason, const QString &pin);
// Whether `password` opens the vault; for confirmations before removing encryption.
bool verifyPassword(const QString &root, const QString &password);

}  // namespace core::vault_ops
