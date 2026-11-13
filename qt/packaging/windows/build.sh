#!/bin/zsh
# build.sh OUT_DIR: cross-builds the Windows x64 stage, portable zip and update payload with llvm-mingw on Linux or macOS.
# Environment: QT_WIN (Qt for llvm-mingw x64), QT_HOST (Qt of the build host, same version), LLVM_MINGW, FFMPEG_WIN
# (ffmpeg.exe), ROOM237_SBOM, ROOM237_UPDATE_PUBKEY, ROOM237_CORE_SOURCES; TOOLCHAIN (cmake file) defaults to toolchain-llvm-mingw.cmake here.
set -euo pipefail
OUT=$1; HERE=${0:A:h}; QT_SRC=${HERE:h:h}; VERSION=$(<"$QT_SRC/VERSION")
TOOLCHAIN=${TOOLCHAIN:-$HERE/toolchain-llvm-mingw.cmake}
export PATH=$LLVM_MINGW/bin:$PATH ROOM237_VERSION="$VERSION"
cmake -S "$QT_SRC/app" -B "$OUT/qt" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DQT_HOST_PATH="$QT_HOST" -DCMAKE_PREFIX_PATH="$QT_WIN" -DQt6_DIR="$QT_WIN/lib/cmake/Qt6" -DROOM237_QA=OFF -DROOM237_CORE_SOURCES="${ROOM237_CORE_SOURCES:-}" -DROOM237_UPDATE_PUBKEY="${ROOM237_UPDATE_PUBKEY:-}"
cmake --build "$OUT/qt" -j "${ROOM237_BUILD_JOBS:-6}"
STAGE=$OUT/stage
[[ ! -e "$STAGE" ]] || { echo "Refusing to replace $STAGE" >&2; exit 2; }; mkdir -p "$STAGE/plugins" "$STAGE/licenses"
cp "$OUT/qt/room237.exe" "$STAGE/"
cp "$OUT/qt/room237-core.exe" "$STAGE/room237-core.exe"
cp "$FFMPEG_WIN" "$STAGE/ffmpeg.exe"
for dll in Qt6Core Qt6Gui Qt6Widgets Qt6Network Qt6Multimedia Qt6MultimediaWidgets Qt6Svg Qt6Concurrent; do cp "$QT_WIN/bin/$dll.dll" "$STAGE/"; done
cp "$QT_WIN"/bin/av*.dll "$QT_WIN"/bin/sw*.dll "$STAGE/"
# The C++ runtime of this compiler: the copy Qt ships is older than the headers the app is compiled against.
cp "$LLVM_MINGW/x86_64-w64-mingw32/bin/libc++.dll" "$LLVM_MINGW/x86_64-w64-mingw32/bin/libunwind.dll" "$STAGE/"
for dir in platforms styles imageformats multimedia tls iconengines; do
  if [[ -d "$QT_WIN/plugins/$dir" ]]; then cp -R "$QT_WIN/plugins/$dir" "$STAGE/plugins/"; fi
done
rm -f "$STAGE/plugins/platforms/qdirect2d.dll"
printf '[Paths]\nPlugins = plugins\n' > "$STAGE/qt.conf"
python3 "$QT_SRC/packaging/windows/collect_imports.py" "$STAGE" "$LLVM_MINGW/bin/llvm-objdump" "$QT_WIN/bin" "$LLVM_MINGW/x86_64-w64-mingw32/bin"
find "$STAGE" -type f \( -name "*.exe" -o -name "*.dll" \) -exec "$LLVM_MINGW/bin/llvm-strip" --strip-debug {} \;
cp "${ROOM237_SBOM:?set ROOM237_SBOM}" "$STAGE/licenses/sbom-core.json"
cp "$QT_SRC/app/resources/icon.ico" "$STAGE/room237.ico"
cp "$QT_SRC/packaging/THIRD_PARTY_NOTICES.md" "$QT_SRC/LICENSE" "$STAGE/licenses/"
python3 "$QT_SRC/packaging/sanitize_diagnostics.py" "$STAGE" "$STAGE/licenses/diagnostic-path-map.json"
tar -C "$STAGE" -czf "$OUT/Room237-$VERSION-windows-x86_64.update.tar.gz" .
(cd "$OUT" && rm -rf Room237 "Room237-$VERSION-windows-x86_64-portable.zip" && cp -R stage Room237 && zip -qry "Room237-$VERSION-windows-x86_64-portable.zip" Room237 && rm -rf Room237)
echo "$STAGE"
