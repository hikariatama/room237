#!/bin/zsh
# build.sh ARCH OUT_DIR: produces Room237.app and the update payload for arm64 or x86_64.
# Environment: QT_ROOT (universal Qt for macOS), FFMPEG_BIN, ROOM237_SBOM, ROOM237_UPDATE_PUBKEY, ROOM237_CORE_SOURCES (the
# unpacked core-sources group of ci/deps.lock.json; fetched at configure time when unset); cmake and ninja on PATH.
# ROOM237_QA=1 builds the test variant with the QA driver (qt/tests).
set -euo pipefail
ARCH=$1; OUT=$2; HERE=${0:A:h}; QT_SRC=${HERE:h:h}
VERSION=$(<"$QT_SRC/VERSION")
mkdir -p "$OUT"; JOBS=${ROOM237_BUILD_JOBS:-6}
export ROOM237_VERSION="$VERSION" MACOSX_DEPLOYMENT_TARGET=13.0
QA_CMAKE=()
if [[ ${ROOM237_QA:-0} == 1 ]]; then
  QA_CMAKE=(-DROOM237_QA=ON)
fi
cmake -S "$QT_SRC/app" -B "$OUT/qt-$ARCH" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DROOM237_QA=OFF -DROOM237_CORE_SOURCES="${ROOM237_CORE_SOURCES:-}" -DROOM237_UPDATE_PUBKEY="${ROOM237_UPDATE_PUBKEY:-}" "${QA_CMAKE[@]}"
cmake --build "$OUT/qt-$ARCH" -j "$JOBS"
APP="$OUT/$ARCH/Room237.app"
[[ ! -e "$OUT/$ARCH" ]] || { echo "Refusing to replace $OUT/$ARCH" >&2; exit 2; }; mkdir -p "$OUT/$ARCH"
cp -R "$OUT/qt-$ARCH/Room237.app" "$APP"
"$QT_ROOT/bin/macdeployqt" "$APP" -verbose=0
# macdeployqt does not discover optional image decoders from direct imports.
mkdir -p "$APP/Contents/PlugIns/imageformats"
for plugin in "$QT_ROOT"/plugins/imageformats/*.dylib; do
  [[ ${plugin:t} == libqpdf.dylib ]] || cp "$plugin" "$APP/Contents/PlugIns/imageformats/"
done
"$QT_ROOT/bin/macdeployqt" "$APP" -verbose=0
cp "$FFMPEG_BIN" "$APP/Contents/MacOS/ffmpeg"
chmod 755 "$APP/Contents/MacOS/ffmpeg"
mkdir -p "$APP/Contents/Resources/licenses"
cp "${ROOM237_SBOM:?set ROOM237_SBOM}" "$APP/Contents/Resources/licenses/sbom-core.json"
cp "$QT_SRC/packaging/THIRD_PARTY_NOTICES.md" "$QT_SRC/LICENSE" "$APP/Contents/Resources/licenses/"
# Parts macdeployqt copies through plugin dependencies that this app never loads.
for unused in PlugIns/quick PlugIns/qmltooling PlugIns/sqldrivers Frameworks/QtPdf.framework Frameworks/QtQuick.framework Frameworks/QtQml.framework Frameworks/QtQmlModels.framework Frameworks/QtQmlMeta.framework Frameworks/QtQmlWorkerScript.framework Frameworks/QtSql.framework PlugIns/imageformats/libqpdf.dylib; do
  rm -rf "$APP/Contents/$unused"
done
# Only the requested architecture ships; universal Qt frameworks would double the size.
find "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" -type f | while read -r file; do
  if lipo -info "$file" 2>/dev/null | grep -q 'Architectures in the fat file'; then
    lipo -thin "$ARCH" "$file" -output "$file.thin" && mv "$file.thin" "$file"
  fi
done
python3 "$QT_SRC/packaging/sanitize_diagnostics.py" "$APP" "$APP/Contents/Resources/licenses/diagnostic-path-map.json"
find "$APP/Contents/MacOS" -type f -exec strip -x {} \;
xattr -cr "$APP"
# Ad-hoc only: arm64 needs a signature to run at all. No Developer ID signature or notarization.
codesign --force --deep --sign - --timestamp=none "$APP"
codesign --verify --deep --strict "$APP"
tar -C "$APP" -czf "$OUT/Room237-$VERSION-macos-$ARCH.update.tar.gz" .
echo "$APP"
