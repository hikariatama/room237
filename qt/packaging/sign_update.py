#!/usr/bin/env python3
"""Minisign-compatible signing for the Qt update feed (pre-hashed Ed25519).

  sign_update.py keygen KEY_FILE PUB_FILE
  sign_update.py public KEY_FILE                         prints the public key line
  sign_update.py sign KEY_FILE FILE [TRUSTED_COMMENT]   writes FILE.minisig
  sign_update.py manifest KEY_FILE VERSION OUT_JSON platform=kind,url,payload [...]

KEY_FILE is one of:
- a raw 40-byte file (8-byte key id + 32-byte Ed25519 seed), as written by keygen for local QA feeds;
- a minisign secret key file ("untrusted comment" line + base64 line), encrypted with scrypt or not;
- the same minisign file base64-encoded once more, as the Tauri updater stores it.
The password of an encrypted key is read from the environment variable named by ROOM237_KEY_PASSWORD_ENV
(default ROOM237_UPDATER_KEY_PASSWORD); unset means an empty password. With ROOM237_EXPECT_PUBKEY set to a public
key line or a file holding one, every command that signs refuses unless the key matches it.
Key material is never printed. Keep KEY_FILE out of the repository and of every artifact.
"""
import base64, binascii, hashlib, json, os, secrets, struct, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization


def _scrypt_params(opslimit, memlimit):
    # libsodium crypto_pwhash_scryptsalsa208sha256 parameter choice, as used by minisign.
    opslimit = max(opslimit, 32768)
    r = 8
    if opslimit < memlimit // 32:
        p = 1
        max_n = opslimit // (r * 4)
        n_log2 = next(n for n in range(1, 63) if (1 << n) > max_n // 2)
    else:
        max_n = memlimit // (r * 128)
        n_log2 = next(n for n in range(1, 63) if (1 << n) > max_n // 2)
        max_rp = min((opslimit // 4) // (1 << n_log2), 0x3FFFFFFF)
        p = max_rp // r
    return 1 << n_log2, r, p


def _minisign_blob(raw):
    text = raw.decode("ascii").strip()
    lines = text.splitlines()
    if len(lines) == 1:
        # The Tauri updater stores the whole minisign key file base64-encoded.
        lines = base64.b64decode(text, validate=True).decode("ascii").strip().splitlines()
    if len(lines) != 2 or not lines[0].startswith("untrusted comment:"):
        raise ValueError("not a minisign secret key file")
    return base64.b64decode(lines[1], validate=True)


def _load_minisign(blob):
    if len(blob) != 158 or blob[:2] != b"Ed" or blob[4:6] != b"B2":
        raise ValueError("unsupported minisign secret key layout")
    kdf, salt = blob[2:4], blob[6:38]
    opslimit, memlimit = struct.unpack("<QQ", blob[38:54])
    keynum = blob[54:158]
    if kdf == b"Sc":
        password = os.environ.get(os.environ.get("ROOM237_KEY_PASSWORD_ENV", "ROOM237_UPDATER_KEY_PASSWORD"), "")
        n, r, p = _scrypt_params(opslimit, memlimit)
        stream = hashlib.scrypt(password.encode(), salt=salt, n=n, r=r, p=p, maxmem=256 * 1024 * 1024, dklen=len(keynum))
        keynum = bytes(a ^ b for a, b in zip(keynum, stream))
    elif kdf != b"\0\0":
        raise ValueError("unsupported minisign key derivation")
    key_id, secret, checksum = keynum[:8], keynum[8:72], keynum[72:104]
    if hashlib.blake2b(b"Ed" + key_id + secret, digest_size=32).digest() != checksum:
        raise ValueError("secret key checksum mismatch (wrong password or damaged key)")
    private = Ed25519PrivateKey.from_private_bytes(secret[:32])
    public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    if public != secret[32:]:
        raise ValueError("secret key does not contain its own public key")
    return key_id, private


def load(path):
    with open(path, "rb") as handle:
        raw = handle.read()
    if len(raw) == 40:
        key_id, private = raw[:8], Ed25519PrivateKey.from_private_bytes(raw[8:])
    else:
        try:
            blob = _minisign_blob(raw)
        except (UnicodeDecodeError, binascii.Error, ValueError):
            raise ValueError("unrecognised key file: expected a raw 40-byte key or a minisign secret key") from None
        key_id, private = _load_minisign(blob)
    expected = os.environ.get("ROOM237_EXPECT_PUBKEY")
    if expected:
        if os.path.isfile(expected):
            with open(expected) as handle:
                expected = handle.read()
        expected = expected.strip().splitlines()[-1].strip()
        if public_b64(key_id, private) != expected:
            raise ValueError("signing key does not match the expected public key")
    return key_id, private


def public_b64(key_id, private):
    public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return base64.b64encode(b"Ed" + key_id + public).decode()


def sign_bytes(key_path, data, trusted):
    key_id, private = load(key_path)
    signature = private.sign(hashlib.blake2b(data, digest_size=64).digest())
    trusted = trusted.encode()
    global_signature = private.sign(signature + trusted)
    return (
        f"untrusted comment: room237 update signature\n"
        f"{base64.b64encode(b'ED' + key_id + signature).decode()}\n"
        f"trusted comment: {trusted.decode()}\n"
        f"{base64.b64encode(global_signature).decode()}\n"
    )


def manifest_text(version, specs):
    platforms = {}
    for spec in specs:
        platform, rest = spec.split("=", 1)
        kind, url, payload = rest.split(",", 2)
        digest, size = hashlib.sha256(), 0
        with open(payload, "rb") as handle:
            for block in iter(lambda: handle.read(1 << 20), b""):
                digest.update(block)
                size += len(block)
        platforms[platform] = {"url": url, "sha256": digest.hexdigest(), "size": size, "kind": kind}
    return json.dumps({"version": version, "notes": "", "platforms": platforms}, indent=2, sort_keys=True) + "\n"


def main(argv):
    command = argv[1] if len(argv) > 1 else ""
    if command == "keygen":
        key_path, pub_path = argv[2], argv[3]
        key_id = secrets.token_bytes(8)
        seed = secrets.token_bytes(32)
        descriptor = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(key_id + seed)
        with open(pub_path, "w") as handle:
            handle.write(public_b64(key_id, Ed25519PrivateKey.from_private_bytes(seed)) + "\n")
    elif command == "public":
        print(public_b64(*load(argv[2])))
    elif command == "sign":
        key_path, path = argv[2], argv[3]
        trusted = argv[4] if len(argv) > 4 else f"file:{os.path.basename(path)}"
        with open(path, "rb") as handle:
            data = handle.read()
        signature = sign_bytes(key_path, data, trusted)
        with open(path + ".minisig", "w") as handle:
            handle.write(signature)
    elif command == "manifest":
        key_path, version, out = argv[2], argv[3], argv[4]
        text = manifest_text(version, argv[5:])
        signature = sign_bytes(key_path, text.encode(), f"version:{version}")
        with open(out, "w") as handle:
            handle.write(text)
        with open(out + ".minisig", "w") as handle:
            handle.write(signature)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
