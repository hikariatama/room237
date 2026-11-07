#!/bin/bash
# build.sh TARGET ROOT [FIXTURES]: builds, verifies and tests the packages of one job after bootstrap.sh TARGET ROOT.
# TARGET: macos-arm64, macos-x86_64, linux, windows. Writes ROOT/work (release_driver.py work directory),
# ROOT/tests/* (offscreen suite matrices) and ROOT/upload (the N packages and reports for the release job).
# The loopback QA feed is signed with a throwaway key made here; no production key is involved.
set -euo pipefail
TARGET=${1:?target}; ROOT=${2:?root}; FIXTURES=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd -P); REPO=$(cd "$HERE/../../.." && pwd -P)
source "$ROOT/env.sh"
umask 077
mkdir -p "$ROOT/qa-key"
[[ -f $ROOT/qa-key/qa.key ]] || python "$REPO/qt/packaging/sign_update.py" keygen "$ROOT/qa-key/qa.key" "$ROOT/qa-key/qa.pub"
umask 022
args=(--source "$REPO" --work "$ROOT/work" --mode rehearsal --key "$ROOT/qa-key/qa.key" --pubkey "$ROOT/qa-key/qa.pub"
      --deps-record "$ROOT/deps/deps-installed.json" --min-free-gib "${ROOM237_MIN_FREE_GIB:-3}" --cache-root "$ROOT/cache")
[[ -n $FIXTURES ]] && args+=(--fixtures "$FIXTURES")
# The offscreen suites need the QA variants; the uploaded packages are the release builds without the QA driver.
if [[ $TARGET != windows ]]; then args+=(--qa); fi
case $TARGET in
  macos-arm64|macos-x86_64)
    arch=${TARGET#macos-}
    mkdir -p "$ROOT/banner"
    python "$REPO/qt/packaging/macos/make_banner.py" "$REPO/qt/packaging/macos/dmg-background.png" "$ROOT/banner/background"
    "$REPO/qt/packaging/release_all.sh" "${args[@]}" --platform macos --arch "$arch" --banner "$ROOT/banner/background.tiff" \
      "--ffmpeg-$arch" "$ROOT/deps/ffmpeg/ffmpeg-macos-$arch"
    platform=macos ;;
  linux)
    "$REPO/qt/packaging/release_all.sh" "${args[@]}" --platform linux --linux-qt "$ROOT/deps/qt-linux" --deps "$ROOT/deps/linux-deps"
    platform=linux ;;
  windows)
    "$REPO/qt/packaging/release_all.sh" "${args[@]}" --platform windows
    platform=windows ;;
  *) echo "unknown target $TARGET" >&2; exit 2 ;;
esac

status=0
if [[ $TARGET != windows ]]; then
  # Unit tests of the core on the build host.
  (cd "$ROOT/work/source/qt/core" && CARGO_TARGET_DIR="$ROOT/cache/cargo-test" cargo test --locked -j "${ROOM237_BUILD_JOBS:-6}") \
    > "$ROOT/cargo-test.log" 2>&1 || status=1
  tail -3 "$ROOT/cargo-test.log"
  if [[ -n $FIXTURES ]]; then
    if [[ $platform == macos ]]; then
      python "$REPO/qt/tests/run_offscreen.py" --work "$ROOT/work" --platform macos --arch "$arch" --qt "$QT_ROOT" \
        --fixtures "$FIXTURES" --out "$ROOT/tests/macos-$arch" || status=1
    else
      for kind in tar appimage; do
        python "$REPO/qt/tests/run_offscreen.py" --work "$ROOT/work" --platform linux --kind "$kind" --qt "$ROOT/deps/qt-linux" \
          --fixtures "$FIXTURES" --out "$ROOT/tests/linux-$kind" || status=1
      done
    fi
  fi
fi

up="$ROOT/upload/$TARGET"
mkdir -p "$up/packages" "$up/reports"
find "$ROOT/work/n/$platform" -maxdepth 1 -type f -exec cp {} "$up/packages/" \;
cp "$ROOT/work/n/sbom-core.json" "$ROOT/work/n/THIRD_PARTY_NOTICES.md" "$ROOT/work/n/trust-public-key.txt" "$up/packages/"
cp "$ROOT/work"/{RELEASE.json,SOURCE_SHA256,source-inventory.json,toolchain.json,commands.json} "$up/reports/"
cp "$ROOT/work"/n/verify-*.json "$up/reports/"
[[ -f $ROOT/cargo-test.log ]] && cp "$ROOT/cargo-test.log" "$up/reports/"
[[ -f $ROOT/apt-installed.txt ]] && cp "$ROOT/apt-installed.txt" "$up/reports/"
for matrix in "$ROOT"/tests/*/matrix.json; do
  [[ -f $matrix ]] && cp "$matrix" "$up/reports/tests-$(basename "$(dirname "$matrix")").json"
done
exit $status
