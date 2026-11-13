#!/bin/bash
# build.sh OUT: tar.gz, AppImage, deb and update payloads for Linux x86_64, built on Ubuntu 22.04 (glibc 2.35).
# Environment: ROOM237_SOURCE (contains qt/), ROOM237_BUILD_CACHE, ROOM237_QT_LINUX (Qt gcc_64 directory),
# ROOM237_DEPS (ffmpeg-linux-x86_64, appimagetool, runtime-x86_64), ROOM237_SBOM, ROOM237_UPDATE_PUBKEY,
# ROOM237_CORE_SOURCES; cmake and ninja on PATH. ROOM237_QA=1 builds the test variant with the QA driver (qt/tests).
set -euo pipefail
OUT=$1; SRC=${ROOM237_SOURCE:?}/qt; VERSION=$(cat "$SRC/VERSION"); QT=${ROOM237_QT_LINUX:?}; DEPS=${ROOM237_DEPS:?}
BUILD=${ROOM237_BUILD_CACHE:?}; JOBS=${ROOM237_BUILD_JOBS:-6}
export ROOM237_VERSION="$VERSION"
QA_CMAKE=(-DROOM237_QA=OFF); QA_SUFFIX=
if [ "${ROOM237_QA:-0}" = 1 ]; then
  QA_CMAKE=(-DROOM237_QA=ON); QA_SUFFIX=-qa
fi
cmake -S "$SRC/app" -B "$BUILD/qt-$VERSION$QA_SUFFIX" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT" -DROOM237_CORE_SOURCES="${ROOM237_CORE_SOURCES:-}" -DROOM237_UPDATE_PUBKEY="${ROOM237_UPDATE_PUBKEY:-}" "${QA_CMAKE[@]}"
cmake --build "$BUILD/qt-$VERSION$QA_SUFFIX" -j "$JOBS"
STAGE=$OUT/stage/room237
[ ! -e "$OUT/stage" ] || { echo "Refusing to replace $OUT/stage" >&2; exit 2; }; mkdir -p "$STAGE/lib" "$STAGE/plugins" "$STAGE/licenses"
cp "$BUILD/qt-$VERSION$QA_SUFFIX/room237" "$STAGE/room237"
cp "$BUILD/qt-$VERSION$QA_SUFFIX/room237-core" "$STAGE/room237-core"
cp "$DEPS/ffmpeg-linux-x86_64" "$STAGE/ffmpeg"
chmod 755 "$STAGE/ffmpeg"
for plugin in platforms/libqxcb.so platforms/libqwayland.so platforms/libqoffscreen.so styles platformthemes/libqxdgdesktopportal.so xcbglintegrations/libqxcb-glx-integration.so wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client tls/libqopensslbackend.so imageformats/libqgif.so imageformats/libqjpeg.so imageformats/libqwebp.so imageformats/libqtiff.so imageformats/libqsvg.so imageformats/libqico.so iconengines/libqsvgicon.so multimedia/libffmpegmediaplugin.so; do
  [ -e "$QT/plugins/$plugin" ] || continue
  mkdir -p "$STAGE/plugins/$(dirname "$plugin")"
  cp -R "$QT/plugins/$plugin" "$STAGE/plugins/$(dirname "$plugin")/"
done
# Resolve the closure against Qt. Libraries every desktop has stay out; the xcb helpers Qt needs and many systems lack stay in.
collect() {
  local queue=("$@")
  while [ ${#queue[@]} -gt 0 ]; do
    local item=${queue[0]}; queue=("${queue[@]:1}")
    while read -r name _ path _; do
      [ -n "${path:-}" ] && [ -e "$path" ] || continue
      case "$path" in
        "$QT"/lib/*|*/libavcodec.so*|*/libavformat.so*|*/libavutil.so*|*/libswresample.so*|*/libswscale.so*|*/libwayland-client.so*|*/libwayland-cursor.so*|*/libwayland-egl.so*|*/libxcb-cursor.so*|*/libxcb-icccm.so*|*/libxcb-image.so*|*/libxcb-keysyms.so*|*/libxcb-render-util.so*|*/libxcb-xinerama.so*|*/libxkbcommon-x11.so*|*/libxcb-util.so*)
          if [ ! -e "$STAGE/lib/$name" ]; then cp -L "$path" "$STAGE/lib/$name"; queue+=("$STAGE/lib/$name"); fi ;;
      esac
    done < <(LD_LIBRARY_PATH="$QT/lib" ldd "$item" 2>/dev/null | grep '=>' || true)
  done
}
mapfile -d '' plugins < <(find "$STAGE/plugins" -type f -name '*.so' -print0)
collect "$STAGE/room237" "$STAGE/room237-core" "$STAGE/ffmpeg" "${plugins[@]}"
find "$STAGE/lib" -type f -name '*.so*' -exec patchelf --set-rpath '$ORIGIN' {} \;
find "$STAGE/plugins" -type f -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../../lib' {} \;
patchelf --set-rpath '$ORIGIN/lib' "$STAGE/room237"
printf '[Paths]\nPlugins = plugins\n' > "$STAGE/qt.conf"
cp "${ROOM237_SBOM:?set ROOM237_SBOM}" "$STAGE/licenses/sbom-core.json"
cp "$SRC/packaging/THIRD_PARTY_NOTICES.md" "$SRC/LICENSE" "$STAGE/licenses/"
strip --strip-unneeded "$STAGE/room237" "$STAGE/room237-core" || true
install -Dm644 "$SRC/app/resources/icon.png" "$STAGE/room237.png"
cat > "$STAGE/dev.dgazizullin.room237.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Room237
Comment=Offline photo and video manager
Exec=room237 %F
Icon=room237
Categories=Graphics;Photography;Viewer;
StartupWMClass=room237
DESK
python3 "$SRC/packaging/sanitize_diagnostics.py" "$STAGE" "$STAGE/licenses/diagnostic-path-map.json"
SYSTEM_DEPS=$(python3 "$SRC/packaging/linux/system_dependencies.py" "$STAGE")
tar -C "$OUT/stage" -czf "$OUT/Room237-$VERSION-linux-x86_64.tar.gz" room237
tar -C "$STAGE" -czf "$OUT/Room237-$VERSION-linux-x86_64.update.tar.gz" .
APPDIR=$OUT/stage/AppDir
rm -rf "$APPDIR"; mkdir -p "$APPDIR/usr"
cp -R "$STAGE" "$APPDIR/usr/room237"
cp "$STAGE/dev.dgazizullin.room237.desktop" "$APPDIR/"
cp "$STAGE/room237.png" "$APPDIR/room237.png"
ln -sf room237.png "$APPDIR/.DirIcon"
cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/room237/room237" "$@"
RUN
chmod 755 "$APPDIR/AppRun"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$DEPS/appimagetool" --runtime-file "$DEPS/runtime-x86_64" "$APPDIR" "$OUT/Room237-$VERSION-linux-x86_64.AppImage" 2>&1 | tail -1
# deb: installs under /opt; updates go through the package manager.
DEB=$OUT/stage/deb
rm -rf "$DEB"; mkdir -p "$DEB/DEBIAN" "$DEB/opt" "$DEB/usr/bin" "$DEB/usr/share/applications" "$DEB/usr/share/icons/hicolor/512x512/apps"
cp -R "$STAGE" "$DEB/opt/room237"
ln -s /opt/room237/room237 "$DEB/usr/bin/room237"
cp "$STAGE/dev.dgazizullin.room237.desktop" "$DEB/usr/share/applications/"
cp "$STAGE/room237.png" "$DEB/usr/share/icons/hicolor/512x512/apps/room237.png"
cat > "$DEB/DEBIAN/control" <<CONTROL
Package: room237
Version: $VERSION
Section: graphics
Priority: optional
Architecture: amd64
Maintainer: Room237 <hikariatama@users.noreply.github.com>
Depends: $SYSTEM_DEPS
Description: Offline photo and video manager
 Room237 keeps photos and videos in plain folders and manages albums, favorites and duplicates.
CONTROL
dpkg-deb -Zgzip --root-owner-group --build "$DEB" "$OUT/room237_${VERSION}_amd64.deb" >/dev/null
rm -rf "$APPDIR" "$DEB"
ls -la "$OUT"/*.tar.gz "$OUT"/*.AppImage "$OUT"/*.deb
