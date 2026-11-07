# Third-party software in Room237

Room237 is licensed under AGPL-3.0-only (LICENSE). The packages also contain:

| Component | Version | License | Source |
|---|---|---|---|
| Qt (Core, Gui, Widgets, Network, Multimedia, MultimediaWidgets, Svg, DBus on Linux; platform, image and TLS plugins) | 6.11.3 | LGPL-3.0-only, dynamically linked and replaceable | https://download.qt.io/official_releases/qt/6.11/6.11.3/ |
| FFmpeg libraries bundled with Qt Multimedia | 7.1 series | LGPL-2.1-or-later | https://ffmpeg.org/download.html |
| FFmpeg command-line tool (`ffmpeg`) | 9.0.2; build details in `ffmpeg -version` | GPL build (GPL-3.0-or-later per its configuration) | https://ffmpeg.org/download.html; builds: https://ffmpeg.martin-riedl.de (macOS), https://github.com/BtbN/FFmpeg-Builds (Linux, Windows) |
| llvm-mingw runtime (libc++, libunwind) on Windows | clang 22 | Apache-2.0 WITH LLVM-exception | https://github.com/mstorsjo/llvm-mingw |
| Tabler Icons (embedded SVG paths) | 3.x | MIT | https://github.com/tabler/tabler-icons |
| Phosphor Icons (two embedded SVG paths) | via react-icons | MIT | https://github.com/phosphor-icons/homepage |
| Lottie animations rendered to WebP (`qt/app/resources/lottie`, sources in `qt/tools/lottie`) | | carried over from the Tauri version of Room237; origin not separately documented | this repository |
| Rust crates in `room237-core` | see `sbom-core.json` | SPDX expression or license-file text per crate in `sbom-core.json`; checksums from Cargo.lock | crates.io |

The packages contain no font files. The interface uses the UI font installed with the operating system (San Francisco
on macOS, Segoe UI on Windows, the desktop font on Linux). The macOS DMG background is a photograph-style image
generated for this project (`qt/packaging/macos/dmg-background.md`); it contains no text besides the room number.

H.264, HEVC and other codecs can be subject to patents in some countries. Room237 only decodes files the user already has.

Qt is dynamically linked. The LGPL-covered libraries may be replaced by compatible builds; no technical restriction on replacement is added by Room237. Source and license terms are available from the Qt source distribution above. macOS bundles use an ad-hoc signature only; changing a library requires reapplying an ad-hoc signature.

The FFmpeg CLI and Qt Multimedia's FFmpeg libraries are separate builds. The CLI build flags and version must be taken from the shipped `ffmpeg -version` output in the verifier report; a GPL CLI does not change the stated license of Qt's separately shipped libav libraries. Decoder availability and patent permissions are separate questions.

Staged Qt/FFmpeg copies use relative diagnostic source filenames in place of the vendor's absolute build-machine filenames. This is a length-preserving string map; the shipped `diagnostic-path-map.json` records offsets and before/after hashes. The map is applied before macOS ad-hoc signing. Library interfaces and loader paths are unchanged.
