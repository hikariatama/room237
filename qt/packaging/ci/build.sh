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
  # Unit tests of the core on the build host, with the pinned ffmpeg on PATH (Linux reads media through it).
  qt_prefix=${QT_ROOT:-$ROOT/deps/qt-linux}
  if [[ $TARGET == linux ]]; then ffmpeg_bin=$ROOT/deps/linux-deps/ffmpeg-linux-x86_64; else ffmpeg_bin=$ROOT/deps/ffmpeg/ffmpeg-$TARGET; fi
  ffmpeg_dir=$ROOT/cache/core-test-bin
  mkdir -p "$ffmpeg_dir"; ln -sf "$ffmpeg_bin" "$ffmpeg_dir/ffmpeg"
  { cmake -S "$ROOT/work/source/qt/core" -B "$ROOT/cache/core-tests" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt_prefix" \
      -DROOM237_CORE_TESTS=ON -DROOM237_CORE_SOURCES="$ROOM237_CORE_SOURCES" &&
    cmake --build "$ROOT/cache/core-tests" -j "${ROOM237_BUILD_JOBS:-6}" &&
    PATH="$ffmpeg_dir:$PATH" QT_QPA_PLATFORM=offscreen "$ROOT/cache/core-tests/room237_core_tests"; } > "$ROOT/core-test.log" 2>&1 || status=1
  tail -3 "$ROOT/core-test.log"
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
[[ -f $ROOT/core-test.log ]] && cp "$ROOT/core-test.log" "$up/reports/"
[[ -f $ROOT/apt-installed.txt ]] && cp "$ROOT/apt-installed.txt" "$up/reports/"
for matrix in "$ROOT"/tests/*/matrix.json; do
  [[ -f $matrix ]] && cp "$matrix" "$up/reports/tests-$(basename "$(dirname "$matrix")").json"
done
exit $status
