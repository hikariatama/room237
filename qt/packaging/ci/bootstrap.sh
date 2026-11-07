#!/bin/bash
# bootstrap.sh TARGET ROOT: installs the pinned toolchain of one build job under ROOT and writes ROOT/env.sh.
# TARGET: macos-arm64, macos-x86_64 (on a macOS host of that architecture), fixtures (macOS arm64), linux, windows
# (as root in ubuntu:22.04, see deps.lock.json "ubuntu-image"), release (any x86_64 Linux host).
# Everything else comes from deps.lock.json (checked by size and SHA-256), requirements.txt (hash-locked) and rustup.
set -euo pipefail
TARGET=${1:?target}; ROOT=${2:?root}
HERE=$(cd "$(dirname "$0")" && pwd -P)
mkdir -p "$ROOT"; ROOT=$(cd "$ROOT" && pwd -P)
case $TARGET in
  macos-arm64)  groups="python-macos-arm64 rust-macos-arm64 qt-macos ffmpeg-macos-arm64"; rust_targets=aarch64-apple-darwin ;;
  macos-x86_64) groups="python-macos-x86_64 rust-macos-x86_64 qt-macos ffmpeg-macos-x86_64"; rust_targets=x86_64-apple-darwin ;;
  fixtures)     groups="python-macos-arm64 ffmpeg-macos-arm64"; rust_targets= ;;
  linux)        groups="python-linux-x86_64 rust-linux-x86_64 qt-linux linux-deps"; rust_targets=x86_64-unknown-linux-gnu ;;
  windows)      groups="python-linux-x86_64 rust-linux-x86_64 qt-linux qt-windows windows-deps"; rust_targets=x86_64-pc-windows-gnullvm ;;
  release)      groups="python-linux-x86_64"; rust_targets= ;;
  *) echo "unknown target $TARGET" >&2; exit 2 ;;
esac
host=$(uname -sm)
case $TARGET in
  macos-arm64|fixtures) [[ $host == "Darwin arm64" ]] ;;
  macos-x86_64) [[ $host == "Darwin x86_64" ]] ;;
  *) [[ $host == "Linux x86_64" ]] ;;
esac || { echo "$TARGET cannot be bootstrapped on $host" >&2; exit 2; }

if [[ $TARGET == linux || $TARGET == windows ]]; then
  export DEBIAN_FRONTEND=noninteractive
  mapfile -t packages < <(grep -v '^#' "$HERE/apt-packages.txt")
  apt-get update -q
  apt-get install -y -q --no-install-recommends "${packages[@]}"
  dpkg-query -W -f '${Package}=${Version}\n' "${packages[@]}" > "$ROOT/apt-installed.txt"
fi

python3 "$HERE/fetch_deps.py" --dest "$ROOT/deps" --cache "${ROOM237_DEPS_CACHE:-$ROOT/downloads}" $groups

"$ROOT/deps/python/bin/python3" -m venv "$ROOT/venv"
"$ROOT/venv/bin/python" -m pip install -q --disable-pip-version-check --require-hashes --only-binary :all: -r "$HERE/requirements.txt"

rust_version=$("$ROOT/venv/bin/python" -c 'import json,sys; print(json.load(open(sys.argv[1]))["versions"]["rust"])' "$HERE/deps.lock.json")
if [[ -n $rust_targets ]]; then
  CARGO_HOME=$ROOT/cargo RUSTUP_HOME=$ROOT/rustup "$ROOT/deps/rustup-init" -y -q --no-modify-path --profile minimal \
    --default-toolchain "$rust_version" --target "$rust_targets"
fi

{
  echo "export ROOM237_CI_ROOT='$ROOT' ROOM237_CI_TARGET='$TARGET'"
  echo "export PATH='$ROOT/venv/bin:$ROOT/cargo/bin':\"\$PATH\" CARGO_HOME='$ROOT/cargo' RUSTUP_HOME='$ROOT/rustup'"
  case $TARGET in
    macos-*) echo "export QT_ROOT='$ROOT/deps/qt-macos' QT_HOST='$ROOT/deps/qt-macos'" ;;
    windows) echo "export QT_WIN='$ROOT/deps/qt-windows' QT_HOST='$ROOT/deps/qt-linux' LLVM_MINGW='$ROOT/deps/llvm-mingw' FFMPEG_WIN='$ROOT/deps/windows-deps/ffmpeg.exe'" ;;
  esac
} > "$ROOT/env.sh"
echo "bootstrap $TARGET: $ROOT/env.sh"
