# Room237 (Qt)

Room237 7.x: a C++/Qt 6 Widgets interface (`app/`) and a Rust core process (`core/`, binary `room237-core`) that
talk over JSON lines on stdin/stdout. There is no web view.

| Path | Contents |
|---|---|
| `VERSION` | Release version, read by CMake and the packaging scripts. |
| `app/` | Interface. `resources/` holds icons, flags and the rendered Lottie animations; text uses the platform's installed UI font. |
| `core/` | `room237-core`: library index, metadata, thumbnails, duplicates, file operations, updates, import of the 6.x settings. |
| `tests/` | Integration suites that drive a built app through its QA port (see `tests/README.md`). |
| `packaging/` | Release driver, platform build scripts, package verifier, update-feed signing, SBOM, notices (see `packaging/BUILD.md`). |
| `tools/` | Asset generators for icons and Lottie animations (see `tools/README.md`). |

## Build for development

Requirements: Qt 6.11 (Widgets, Network, Multimedia, MultimediaWidgets, Svg; DBus on Linux), CMake ≥ 3.21 and Ninja,
Rust ≥ 1.90, and an `ffmpeg` binary. `packaging/ci/bootstrap.sh` installs the exact versions the release packages use
(`packaging/BUILD.md`).

```sh
cmake -S qt/app -B out/app -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT"
cmake --build out/app
cargo build --release --locked --manifest-path qt/core/Cargo.toml --target-dir out/core
```

A Debug build (`-DCMAKE_BUILD_TYPE=Debug`, `cargo build` without `--release`) includes the QA driver that the
integration suites talk to. For a fast build with it, pass `-DROOM237_QA=ON` to CMake and `--features qa` to Cargo.
Release packages contain neither.

The app starts `room237-core` from its own executable directory, and the core looks for `ffmpeg` next to itself.
On macOS copy both into `out/app/Room237.app/Contents/MacOS/`; elsewhere put them next to `room237`.
Release builds take the version from `qt/VERSION` (`ROOM237_VERSION`); keep `core/Cargo.toml` at the same version.

## Run

```sh
Room237 [--root LIBRARY_DIR] [--profile PROFILE_DIR] [--legacy-profile TAURI_PROFILE_DIR]
```

Without `--profile` the app uses the default profile directory (`Room237 --print-default-paths` prints it) and, on the
first start, imports the settings of the Tauri-based 6.x app from its profile once, read-only. An explicit `--profile`
is an isolated installation: it imports only a profile named with `--legacy-profile`.

## Test

```sh
cargo test --locked --manifest-path qt/core/Cargo.toml
```

The integration suites in `tests/` need an app and core built with the QA driver, a fenced work directory and, for
some, synthetic media; see `tests/README.md`. `tests/run_offscreen.py` runs all offscreen suites against the QA variants
of a release work directory (`release_driver.py --qa`).

## Updates

The app checks `https://github.com/hikariatama/room237/releases/latest/download/qt-update.json` and its minisign
signature (`qt-update.json.minisig`) before downloading anything. The feed is separate from the 6.x feed `latest.json`.
The trusted public key is the 6.x updater key (`core/src/update.rs`, `ROOM237_UPDATE_PUBKEY` at build time); the feed
must be signed with the matching private key. Feed signatures and platform code signing (Developer ID, Authenticode)
are independent; current packages have neither Developer ID nor Authenticode signatures.

## QA mode and the root fence

The QA driver exists only in Debug builds and builds made with `-DROOM237_QA=ON` / `--features qa`; release packages
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
