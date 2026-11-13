# Integration tests

Each suite launches a Room237 built with the QA driver (a Debug build, or `-DROOM237_QA=ON`; the app binary inside
the bundle on macOS) and talks to it over a local port (`qa.py`). Every suite must run in its own disposable fence:

```sh
export ROOM237_QA_FENCE=/abs/path/to/new/empty/dir   # canonical absolute path, created beforehand
export HOME=$ROOM237_QA_FENCE/home TMPDIR=$ROOM237_QA_FENCE/tmp/ QT_QPA_PLATFORM=offscreen
mkdir -p "$HOME" "$TMPDIR"
python3 qt/tests/test_isolation.py "$APP" "$ROOM237_QA_FENCE/work"
```

`WORK` must be new and inside the fence; suites write `results.json` there and exit non-zero on any failed check.
Python 3.11+ with Pillow is required (`cryptography` for `test_update.py`). Fixtures:

- `gen_format_fixtures.py OUT_DIR` writes synthetic media in every supported format plus `formats.expected.json`
  (HEIC generation needs macOS `sips`; `ffmpeg` on `PATH`). Copy the result into the fence before use.
- `fixtures/tauri-profile/` is a synthetic profile written by the 6.x Tauri app's QA build.

## Core stand-ins

A QA build talks to a separate process instead of its built-in core when `ROOM237_QA_CORE_PROCESS` names one; the
process speaks JSON lines on stdin/stdout. `Room237 --qa-core-server --profile DIR` serves the real core that way.
`listing_proxy.py` sits between the two to hold and reorder answers; `test_root_fence.py` forges replies.

## Offscreen suites (macOS, Linux)

| Suite | Arguments | Checks |
|---|---|---|
| `test_core_package.py` | `CORE_BINARY WORK` | `room237-core --version`, and the core protocol of `Room237 --qa-core-server` without the window |
| `test_qa_helpers.py` | `FENCE_PROBE WORK` | fence path rules in `qa_fence.cpp` (build `fence_probe/` with `-DROOM237_SOURCE_ROOT=<repo>`) |
| `test_root_fence.py` | `APP WORK OUTPUT_DIR` | QA fence refusals at startup, root change and core calls |
| `test_isolation.py` | `APP WORK` | explicit profiles stay isolated; Tauri profile import receipts |
| `test_external_album.py` | `APP WORK` | albums renamed, added or removed outside the app |
| `test_listing_order.py` | `APP_BUNDLE_OR_DIR WORK` | late or stale core listings never replace newer ones |
| `test_formats.py` | `APP FIXTURES EXPECTED_JSON WORK [--encrypted]` | thumbnails, sizes and viewer for every format; `--encrypted` checks them in an encrypted root |
| `test_hardening.py` | `APP WORK` | security hardening: setup, recovery key, lock and unlock, decoy password, copies for other apps, nested folder, removal |
| `test_processing.py` | `APP WORK` | the processing screen covers the room during indexing and encryption, takes clicks and shortcuts, saves worker counts |
| `test_functional.py` | `APP FIXTURES WORK` | albums, favorites, selection, clipboard import, move, delete, dates, viewer, duplicates, external changes |
| `test_near_duplicates.py` | `APP WORK` | near-duplicate precision and recall against ground truth |
| `test_duplicate_positive.py` | `APP WORK` | exact duplicates; same stem with different extensions |
| `test_album_create_scope.py` | `APP WORK` | creating an album rescans only that album |
| `test_fonts.py` | `APP WORK` | no font files in the package or resources; text uses the platform UI font (macOS: widths of the former SF Pro Text) |
| `test_migration.py` | `APP FIXTURES WORK [--profile DIR]` | import of a 6.x Tauri profile |
| `test_legacy_discovery.py` | `APP WORK --offscreen-profile DIR` | the 6.x profile is found at the platform default location |
| `test_update.py` | `PLATFORM WORK - SIGN_TOOL INSTALL_N PAYLOAD_N1 --fixture IMAGE` | signed local N → N+1 update with rollback cases on disposable installed copies |

`test_update.py` needs the N install and the N+1 update payload from one release run (`packaging/BUILD.md`); with
`KEY=-` it signs its own feed with a throwaway key. Under offscreen Qt both builds also need the `qoffscreen`
platform plugin.

`run_offscreen.py --work WORK --platform macos --arch arm64 --qt QT_ROOT --fixtures DIR --out NEW_DIR` (on Linux:
`--platform linux --kind tar|deb|appimage`) runs these suites against the QA variants of the N and N+1 packages of a
`release_driver.py --qa` work directory, each in a new fence, and writes `OUT/matrix.json`. The packaging workflow runs it on every build.

## Visible-window suites (macOS only)

`test_input.py`, `test_input_guard.py`, `test_native_panel.py` and the visible variant of `test_functional.py` drive
the real window with synthetic input. They refuse to run without a written authorization file
(`ROOM237_SLOT_AUTHORIZATION`, see `slot_contract.authorization`) that confirms an unused, unlocked machine, and they
use only the app's own windows. The helpers are compiled from the Swift files here (`swiftc qa_place.swift -o qa_place`
and so on) and passed as arguments (`qa_input`, `qa_dragsource`, `ax_probe`) or through the environment (`QA_PLACE`,
`QA_GUARD`, `QA_WINID`, `QA_OCCLUDER`, `ROOM237_GATE_TOOL` for `passive_window`); `ROOM237_SLOT_VIEWPORT=x,y,w,h`
sets the window frame.
