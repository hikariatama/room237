# Release checklist

## 7.0.0 notes

First release of the C++/Qt 6 app; the Rust core is the 6.x core without Tauri. Settings of the 6.x app are imported
read-only on first start; libraries need no conversion. 6.x installs are not offered this update: version 7 reads
`qt-update.json`, version 6 reads `latest.json`.

Packages: macOS arm64 and x86_64 (DMG, ad-hoc signature, not notarized), Linux x86_64 (AppImage, deb, tar.gz),
Windows x64 (NSIS installer, portable zip, unsigned). No ARM64 Linux or Windows builds.

## Before publishing

1. Run the `release` workflow (`BUILD.md`, "Release on GitHub") on the release commit. It builds and tests every
   platform, signs `qt-update.json` with the production key and creates a draft release targeting that commit.
2. Check the workflow results: verifier reports, the core unit tests and the offscreen suite matrices of macOS arm64,
   macOS x86_64 and Linux (tar and AppImage), including `test_update.py` N → N+1 on each update payload kind.
3. Open items the automated runs do not cover; decide on each before publishing:
   - Windows: installer, first start, update, Explorer drag and drop, clipboard and Direct3D video on a real
     Windows machine (only static PE/NSIS checks exist).
   - Linux: AppImage with FUSE on a native x86_64 kernel (the suites use extract-and-run).
   - Platform signing: none today; Developer ID + notarization and Authenticode are separate decisions.
4. Review the draft assets: every package, `SHA256SUMS`, `trust-public-key.txt`, `sbom-core.json`,
   `THIRD_PARTY_NOTICES.md`, `qt-update.json` and `qt-update.json.minisig`. No QA feed or `qa-feed-public-key.txt`.
5. Before publishing, remove the "not published yet" note from the Download section of the root `README.md`.
6. Publish the draft. This creates the tag `room237-v<version>` on the target commit and makes the feed reachable
   through `releases/latest/download/`. The landing page links to the latest release page.
