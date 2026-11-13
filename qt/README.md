# Room237 (Qt)

Room237 7.x: a C++/Qt 6 Widgets interface (`app/`) with the library core (`core/`) linked into the same process.
There is no web view.

| Path | Contents |
|---|---|
| `VERSION` | Release version, read by CMake and the packaging scripts. |
| `app/` | Interface. `resources/` holds icons, flags and the rendered Lottie animations; text uses the platform's installed UI font. |
| `core/` | Library index, metadata, thumbnails, duplicates, file operations, updates, import of the 6.x settings; `room237-core`, the update helper without Qt; unit tests in `core/tests`. |
| `tests/` | Integration suites that drive a built app through its QA port (see `tests/README.md`). |
| `packaging/` | Release driver, platform build scripts, package verifier, update-feed signing, SBOM, notices (see `packaging/BUILD.md`). |
| `tools/` | Asset generators for icons and Lottie animations (see `tools/README.md`). |

## Build for development

Requirements: Qt 6.11 (Widgets, Network, Multimedia, MultimediaWidgets, Svg, Concurrent; DBus on Linux), CMake ≥ 3.21,
Ninja, Python 3, Perl, make, `pkg-config` (`packaging/ci/requirements.txt` installs `pkgconf`) and an `ffmpeg` binary;
on macOS also the Swift compiler of the Xcode command line tools (the Secure Enclave factor uses CryptoKit), on Linux the
libudev and tpm2-tss headers (`-DROOM237_TPM=OFF` builds without the TPM factor). The core compiles SQLite, libwebp, Monocypher, zlib, libfido2, libcbor and OpenSSL's libcrypto from
the sources pinned in
`packaging/ci/deps.lock.json`: CMake downloads and checks them at configure time, or takes an unpacked copy with
`-DROOM237_CORE_SOURCES=DIR` (`packaging/ci/fetch_deps.py --dest D core-sources` writes `D/core-sources`).
`packaging/ci/bootstrap.sh` installs the exact versions the release packages use (`packaging/BUILD.md`).

```sh
cmake -S qt/app -B out/app -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT"
cmake --build out/app
```

A Debug build (`-DCMAKE_BUILD_TYPE=Debug`) includes the QA driver that the integration suites talk to. For a fast
build with it, pass `-DROOM237_QA=ON`. Release packages do not contain it.

The build puts `room237-core` next to the app executable. The core looks for `ffmpeg` in the same directory: on macOS
copy it into `out/app/Room237.app/Contents/MacOS/`, elsewhere next to `room237`. The version comes from `qt/VERSION`.

## Security hardening

With `privacy.hardening` on (Advanced settings → Privacy), a root can be encrypted. Its originals, previews, file names
and album databases are stored encrypted (XChaCha20-Poly1305); the library index of an encrypted root is kept in
memory only. The master key is wrapped once per second factor with a key derived from the password (Argon2id, 256 MiB,
3 passes) and the factor's secret, and once more with a recovery key shown at setup. The password is never stored; the
key derived from it stays in memory until the app quits, so unlocking after a lock needs only the factor.

| Factor | Platform | State |
|---|---|---|
| Secure Enclave key, Touch ID or the Mac's password (`.biometryCurrentSet` or `.devicePasscode`) | macOS | implemented |
| Windows Hello key credential (KeyCredentialManager); S from its signature over a stored challenge | Windows | implemented; compiled, not yet run on Windows |
| TPM 2.0: a secret sealed under the owner storage key, the PIN as its authorization, in an encrypted session (tpm2-tss, loaded at run time) | Linux | implemented; tested against swtpm |
| FIDO2 security key: a non-resident credential with hmac-secret, its PIN when it has one (libfido2) | all; Windows through `webauthn.dll` | implemented; not yet tried with a physical key |

`-DROOM237_FIDO=OFF` builds without security keys. On Linux, reading a key needs access to its `/dev/hidraw*` node,
which systemd's udev rules grant to the logged-in user.

QA builds also offer a `test` factor without a prompt, for the integration suites. `core/src/vault.h` describes the
file formats; `core/src/vault_ops.h` the commands. Removing the encryption restores every name, content, modification
time and extended attribute and deletes the vault file.

## Run

```sh
Room237 [--root LIBRARY_DIR] [--profile PROFILE_DIR] [--legacy-profile TAURI_PROFILE_DIR]
```

Without `--profile` the app uses the default profile directory (`Room237 --print-default-paths` prints it) and, on the
first start, imports the settings of the Tauri-based 6.x app from its profile once, read-only. An explicit `--profile`
is an isolated installation: it imports only a profile named with `--legacy-profile`.

## Test

```sh
cmake -S qt/core -B out/core-tests -G Ninja -DCMAKE_PREFIX_PATH="$QT_ROOT" -DROOM237_CORE_TESTS=ON
cmake --build out/core-tests && out/core-tests/room237_core_tests
```

The integration suites in `tests/` need an app built with the QA driver, a fenced work directory and, for
some, synthetic media; see `tests/README.md`. `tests/run_offscreen.py` runs all offscreen suites against the QA variants
of a release work directory (`release_driver.py --qa`).

## Updates

The app checks `https://github.com/hikariatama/room237/releases/latest/download/qt-update.json` and its minisign
signature (`qt-update.json.minisig`) before downloading anything. The feed is separate from the 6.x feed `latest.json`.
The trusted public key is the 6.x updater key (`core/src/update.cpp`, `-DROOM237_UPDATE_PUBKEY` at build time); the feed
must be signed with the matching private key. The swap of the installed copy runs in `room237-core`, which the app
copies into the profile so that it keeps running while the installation is replaced; 7.0 installs check the update
with `room237-core --version` before swapping it in. Feed signatures and platform code signing (Developer ID, Authenticode)
are independent; current packages have neither Developer ID nor Authenticode signatures.

## QA mode and the root fence

The QA driver exists only in Debug builds and builds made with `-DROOM237_QA=ON`; release packages
do not contain it and ignore every `ROOM237_QA_*` variable. In such a build the driver is active only when
`ROOM237_QA_PORT` is set. It then also requires `ROOM237_QA_FENCE`: the full
absolute path of an existing disposable directory. The app checks canonical absolute paths against it at startup, on
profile load, migration, root change and before core calls; a refusal leaves the active root and the saved profile
unchanged. Component names, prefixes and `..` are not accepted; a symlink may point into the fence, never out of it.
Without `ROOM237_QA_PORT` there is no fence.

- Give the QA build explicit `--root`, `--profile` and an empty `--legacy-profile` inside the fence, and a synthetic
  `HOME` there as well.
- The fence is a guard for test runs, not a sandbox: the core refuses directory listings and new preload jobs outside
  it, but direct core calls without `ROOM237_QA_PORT` are not restricted, and concurrent filesystem changes after a
  check are not prevented.
- The updater is off in QA mode except with `ROOM237_QA_UPDATE_FIXTURE` (a JSON fixture whose feed, staging, install,
  profile and key paths all lie inside the fence). Outside QA mode the variable has no effect.
- macOS only: the QA command `native_directory_open` drives the real `NSOpenPanel` of the app's own active window and
  confirms only a path it has read back from the panel; other platforms refuse it.
