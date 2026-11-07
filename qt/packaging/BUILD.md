# Release packages

`release_driver.py` (entry point `release_all.sh`) builds and verifies the packages of one platform from a snapshot
of `qt/`: macOS arm64 and x86_64 on macOS, Linux x86_64 on Ubuntu 22.04, Windows x64 cross-compiled with llvm-mingw
on Linux (or macOS). It builds version N from `qt/VERSION` and N+1 (next patch) so that updates can be tested, signs a
loopback QA feed with a throwaway key and publishes nothing. `assemble_release.py` then joins the packages of all
platforms into one release directory with the production update feed.

The GitHub workflows run exactly these scripts on GitHub-hosted runners:

- `.github/workflows/packages.yml` (pull requests, pushes to `master`, manual): builds, verifies and tests every
  platform and assembles the release layout with an unsigned feed. It uses no secrets.
- `.github/workflows/release.yml` (manual only): runs `packages.yml` for the selected commit, signs the feed with the
  production key and creates a **draft** release targeting that commit. It never publishes, never marks a release as
  latest and creates no tag; publishing the draft stays a manual decision.

Neither workflow has run on GitHub yet; until it has, treat them as prepared, not as proven.

## Pinned inputs

Every build input is pinned in the repository and checked before use:

| File | Contents |
|---|---|
| `ci/deps.lock.json` | URL, size and SHA-256 of Qt 6.11.3 (official online repository archives for macOS, Linux gcc_64 and Windows llvm-mingw), Python 3.13 (python-build-standalone), `rustup-init` 1.29.3, FFmpeg 9.0.2 command-line builds (macOS: Martin Riedl, Linux and Windows: BtbN month-end build), llvm-mingw 20260616 (UCRT, Linux host), appimagetool 1.9.1 and the AppImage type-2 runtime 20251108, and the digest of the `ubuntu:22.04` image |
| `ci/requirements.txt` | Python packages with every published wheel hash (`pip --require-hashes --only-binary :all:`), including CMake 4.4.4 and Ninja 1.13.2 |
| `ci/apt-packages.txt` | Ubuntu 22.04 packages for the Linux and Windows jobs (the installed versions are recorded per run) |

Rust 1.99.1 is installed by `rustup-init`, which checks every component against the signed channel manifest.
Qt archives were compared with complete Qt installations of the same version: every shared file is byte-identical
apart from the prefix in `lib/pkgconfig/*.pc`.

`ci/fetch_deps.py --list` prints the lock. To update an input, change its URL, size and SHA-256 together.

## Build locally

```sh
qt/packaging/ci/bootstrap.sh macos-arm64 "$ROOT"            # or macos-x86_64 on an Intel Mac
qt/packaging/ci/build.sh macos-arm64 "$ROOT" "$FIXTURES"     # FIXTURES: qt/tests/gen_format_fixtures.py output
```

`bootstrap.sh` installs everything under `ROOT` (downloads are cached in `ROOM237_DEPS_CACHE` when set) and writes
`ROOT/env.sh`; it installs nothing elsewhere. `build.sh` makes the DMG background, runs the driver with `--qa`,
`cargo test` and the offscreen suites (`qt/tests/run_offscreen.py`) against the QA variants, and copies the N release
packages and reports to `ROOT/upload`.
Linux and Windows run in the pinned Ubuntu image, the same way the workflow does, from a clean export of the commit:

```sh
mkdir "$EXPORT" && git archive HEAD | tar -x -C "$EXPORT"
docker run --rm --platform linux/amd64 -v "$EXPORT:/src:ro" -v "$ROOT:/r" -v "$FIXTURES_DIR:/fixtures:ro" \
  ubuntu:22.04@sha256:076e7bf82319619f28dd0aa9c02ab34b055ad37100395b4f49a2ffba04fb5e11 \
  bash -c '/src/qt/packaging/ci/bootstrap.sh linux /r/linux && /src/qt/packaging/ci/build.sh linux /r/linux /fixtures/formats'
```

Use `windows` instead of `linux` for the Windows packages (no fixtures argument). The driver can also be called
directly; `release_driver.py --help` lists every input. Notes:

- `--work` must be new, absolute and outside the source. The driver copies `qt/` (without `target/` and
  `__pycache__/`) into `WORK/source`, writes `source-inventory.json` (SHA-256 and mode of every file) and its hash
  `SOURCE_SHA256`. `--mode final --freeze-hash HASH` repeats a build and refuses if the tree changed.
- Every step checks free disk space: `--min-free-gib` (default 15, `ROOM237_MIN_FREE_GIB`). The workflows use 3,
  which fits GitHub-hosted runners; keep the default on a shared workstation.
- `--jobs` (default 6, `ROOM237_BUILD_JOBS`) limits parallel compilation; builds run under `nice -n 10`.
- `WORK/n` and `WORK/n1` hold the packages, the signed QA feed, `trust-public-key.txt`, `SHA256SUMS`,
  `sbom-core.json`, `THIRD_PARTY_NOTICES.md` and verifier reports. `commands.json`, `toolchain.json` (including the
  pinned inputs), `disk.jsonl`, `RELEASE.json`, `REPRODUCE.txt` and per-step logs record what ran. Packages are not
  byte-for-byte reproducible (timestamps, container metadata).
- `--qa` (macOS, Linux) also builds `WORK/n/qa` and `WORK/n1/qa`: the same packages with the QA driver in the app and
  the core, for the offscreen suites only. They are not verified, signed into a feed or uploaded. The verifier checks
  that the release packages contain no QA hooks.

Package names (N = version):

| Platform | Packages | Update payload (feed key) |
|---|---|---|
| macOS | `Room237-N-macos-arm64.dmg`, `Room237-N-macos-x86_64.dmg` | `Room237-N-macos-<arch>.update.tar.gz` (`darwin-aarch64`, `darwin-x86_64`) |
| Linux | `Room237-N-linux-x86_64.AppImage`, `room237_N_amd64.deb`, `Room237-N-linux-x86_64.tar.gz` | `Room237-N-linux-x86_64.update.tar.gz` (`linux-x86_64`), the AppImage (`linux-appimage-x86_64`) |
| Windows | `Room237-N-windows-x86_64-setup.exe`, `Room237-N-windows-x86_64-portable.zip` | `Room237-N-windows-x86_64.update.tar.gz` (`windows-x86_64`) |

The deb is updated through the package manager.

### Reusing an accepted core

When `qt/core` is byte-identical to an accepted release, the core binaries can be shipped instead of recompiled:

```sh
python3 qt/packaging/core_reuse.py prepare "$ACCEPTED_WORK" "$CORE_INPUTS"
qt/packaging/release_all.sh … --reuse-core "$CORE_INPUTS/manifest.json" [--share-identical-with "$ACCEPTED_WORK"]
```

`prepare` takes `room237-core` for N and N+1 of every platform from the accepted work directory, checks each against
the released archives, its version string, the production key and the absence of a QA key, and records the SHA-256
of every `qt/core` file. The driver refuses before running cargo if any of that differs. `--share-identical-with`
turns byte-identical outputs into APFS clones of the accepted files.

## Release on GitHub

1. Create the environment `release` (Settings → Environments) with required reviewers, and add its secrets:
   - `ROOM237_UPDATER_PRIVATE_KEY`: the content of the production updater key file (the Tauri-format key: one line
     of base64 around a minisign secret key; a plain minisign secret key file also works);
   - `ROOM237_UPDATER_KEY_PASSWORD`: the key password, only if the key has one.
   No other job or workflow reads these secrets; pull requests never see them.
2. Run the `release` workflow on the commit to release, with `version` equal to `qt/VERSION`. It refuses if a release,
   draft or tag `room237-v<version>` already exists.
3. Review the draft: every package, `SHA256SUMS`, `trust-public-key.txt`, `sbom-core.json`, `THIRD_PARTY_NOTICES.md`,
   `qt-update.json` and `qt-update.json.minisig`. Publishing it creates the tag on the target commit.

The app reads the feed from `releases/latest/download/`, so installed copies see an update only after the release is
published and is the latest one; a draft is invisible to them.

## Keys

- The app trusts the updater key compiled into `room237-core`: by default the 6.x key `RWSptBX…` in
  `qt/core/src/update.rs` (`--trust-pubkey` builds with another one). It is written to `trust-public-key.txt` and
  `RELEASE.json`.
- `--key`/`--pubkey` of the driver is a throwaway pair for the loopback QA feed (`sign_update.py keygen`); the driver
  refuses it if it equals the trusted key. Shipped apps reject that feed; only the QA variants accept it, through a
  fenced fixture.
- The release feed is signed by `assemble_release.py --key KEY`. `sign_update.py` reads a raw 40-byte key, a
  minisign secret key file or the base64-wrapped Tauri form; an encrypted key takes its password from
  `ROOM237_UPDATER_KEY_PASSWORD` (unset means empty). Before signing it checks that the key matches
  `trust-public-key.txt`, and afterwards `verify_feed.py --url-prefix` checks the signature, the trusted comment, every
  platform key and the size and SHA-256 of every payload. To sign locally:

  ```sh
  qt/packaging/assemble_release.py --key /private/path/room237.key --repo hikariatama/room237 \
    --tag room237-v$(cat qt/VERSION) --out "$RELEASE" "$ROOT"/*/upload/*/packages
  ```

- Private keys never go into the repository, logs, reports or packages.

## DMG background

`macos/make_banner.py macos/dmg-background.png BASE` scales the stored photograph to 640x400 and 1280x800, adds the
arrow between the icon positions and joins both into `BASE.tiff` (macOS `tiffutil`). It stops if Finder's black or
white icon labels would fall below a 3:1 contrast. The photograph and how it was made: `macos/dmg-background.md`.

## Verification

`verify_packages.py` runs after every platform build:

- every shipped Mach-O/ELF/PE file, its rpaths and import closure, Qt plugins, codec libraries, `ffmpeg`, icons,
  notices and the Rust SBOM;
- no builder paths in shipped code: the work, source and SDK directories (`--forbid`), the builder's `~/.cache` and
  cloud folders, including UTF-16 strings in PE files;
- DMGs are mounted read-only (`hdiutil -readonly -nobrowse -noautoopen`) to check the Applications link, `.DS_Store`,
  the background and the app hash; the NSIS installer and the AppImage layout are checked statically.

`sanitize_diagnostics.py` replaces absolute vendor source file names inside the staged Qt and FFmpeg libraries with
relative names of the same length before signing; the map is shipped as `licenses/diagnostic-path-map.json`.

Signing: macOS packages are ad-hoc signed only (no Developer ID, no notarization); Windows files carry no Authenticode
signature.

## Known limits

- Windows packages are checked statically only. Installer, Explorer drag and drop, clipboard and Direct3D need a real
  Windows machine.
- The AppImage is tested in extract-and-run mode; FUSE mounting on a native x86_64 kernel is not covered by the scripts.
- No fonts are bundled. macOS uses the system UI font (San Francisco) at the text optical size with a tracking
  correction toward the advances of SF Pro Text; Windows uses Segoe UI and Linux the desktop font, falling back to
  Noto Sans or DejaVu Sans, so widths and line heights differ between platforms.

Decoding routes per format: [CODECS.md](CODECS.md).
