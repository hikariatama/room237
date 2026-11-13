// Secure Enclave factor. The private key never leaves the enclave; its data representation is an encrypted blob that
// only this Mac's enclave can use, and every use needs Touch ID or the Mac's password. S is derived from an ECDH
// agreement between that key and a public key stored next to the blob.
import CryptoKit
import Foundation
import LocalAuthentication
import Security

private func report(_ error: Error, _ buffer: UnsafeMutablePointer<CChar>?, _ capacity: Int32) {
    guard let buffer = buffer, capacity > 0 else { return }
    let text: String
    if let la = error as? LAError {
        switch la.code {
        case .userCancel, .systemCancel, .appCancel: text = "cancelled"
        case .biometryLockout: text = "lockout"
        case .authenticationFailed: text = "authentication failed"
        default: text = la.localizedDescription
        }
    } else if let crypto = error as? CryptoKitError, case .underlyingCoreCryptoError = crypto {
        // A key bound to the current set of fingerprints stops working once the set changes.
        text = "invalidated"
    } else {
        text = error.localizedDescription
    }
    _ = text.withCString { strlcpy(buffer, $0, Int(capacity)) }
}

private func derive(_ shared: SharedSecret) -> Data {
    shared.withUnsafeBytes { bytes in
        var hasher = SHA256()
        hasher.update(data: Data("room237 se v1|".utf8))
        hasher.update(bufferPointer: bytes)
        return Data(hasher.finalize())
    }
}

@_cdecl("room237_se_available")
public func room237_se_available() -> Int32 {
    return SecureEnclave.isAvailable ? 1 : 0
}

/// Creates the enclave key. Writes its blob and the stored public key (x963) and S (32 bytes).
@_cdecl("room237_se_create")
public func room237_se_create(_ reason: UnsafePointer<CChar>, _ blob: UnsafeMutablePointer<UInt8>, _ blobCapacity: Int32, _ blobLength: UnsafeMutablePointer<Int32>,
                              _ peer: UnsafeMutablePointer<UInt8>, _ secret: UnsafeMutablePointer<UInt8>, _ error: UnsafeMutablePointer<CChar>?, _ errorCapacity: Int32) -> Int32 {
    var failure: Unmanaged<CFError>?
    guard let access = SecAccessControlCreateWithFlags(nil, kSecAttrAccessibleWhenUnlockedThisDeviceOnly, [.privateKeyUsage, .biometryCurrentSet, .or, .devicePasscode], &failure) else {
        report(failure!.takeRetainedValue() as Error, error, errorCapacity)
        return 0
    }
    do {
        let context = LAContext()
        context.localizedReason = String(cString: reason)
        let key = try SecureEnclave.P256.KeyAgreement.PrivateKey(accessControl: access, authenticationContext: context)
        let ephemeral = P256.KeyAgreement.PrivateKey()
        let shared = try key.sharedSecretFromKeyAgreement(with: ephemeral.publicKey)
        let data = key.dataRepresentation
        guard data.count <= Int(blobCapacity) else { return 0 }
        data.copyBytes(to: blob, count: data.count)
        blobLength.pointee = Int32(data.count)
        let public963 = ephemeral.publicKey.x963Representation
        public963.copyBytes(to: peer, count: min(public963.count, 65))
        derive(shared).copyBytes(to: secret, count: 32)
        return 1
    } catch let thrown {
        report(thrown, error, errorCapacity)
        return 0
    }
}

/// Reproduces S from the blob and stored public key; the system shows its prompt with `reason`.
@_cdecl("room237_se_agree")
public func room237_se_agree(_ blob: UnsafePointer<UInt8>, _ blobLength: Int32, _ peer: UnsafePointer<UInt8>, _ reason: UnsafePointer<CChar>, _ secret: UnsafeMutablePointer<UInt8>,
                             _ error: UnsafeMutablePointer<CChar>?, _ errorCapacity: Int32) -> Int32 {
    do {
        let context = LAContext()
        context.localizedReason = String(cString: reason)
        let key = try SecureEnclave.P256.KeyAgreement.PrivateKey(dataRepresentation: Data(bytes: blob, count: Int(blobLength)), authenticationContext: context)
        let other = try P256.KeyAgreement.PublicKey(x963Representation: Data(bytes: peer, count: 65))
        let shared = try key.sharedSecretFromKeyAgreement(with: other)
        derive(shared).copyBytes(to: secret, count: 32)
        return 1
    } catch let thrown {
        report(thrown, error, errorCapacity)
        return 0
    }
}
