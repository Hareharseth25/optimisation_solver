#!/usr/bin/env bash
# Builds KAIRO-<version>-x86_64.AppImage from a deployed Linux install tree.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKAIRO_DESKTOP_DEPLOY=ON -DCMAKE_PREFIX_PATH=<Qt>/gcc_64
#   cmake --build build --target KAIRO
#   cmake --install build --prefix dist
#   desktop/packaging/linux/make_appimage.sh dist [appimagetool]
#
# The install tree already holds everything KAIRO needs from Qt (bin/KAIRO
# with RPATH $ORIGIN/../lib, lib/, plugins/, bin/qt.conf), so the AppDir is
# that tree under usr/ plus the desktop entry, the icon and AppRun.
# appimagetool is downloaded only if no path to it is given.
set -euo pipefail

dist="$(cd "${1:?usage: make_appimage.sh <install-prefix> [appimagetool]}" && pwd)"
tool="${2:-}"
version="${KAIRO_VERSION:-0.1.0}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

appdir="$work/KAIRO.AppDir"
mkdir -p "$appdir/usr"
cp -a "$dist"/. "$appdir/usr/"
# The CLI is not part of the desktop AppImage.
rm -f "$appdir/usr/bin/optimsolver"
cp "$dist/share/applications/org.kairo.desktop.desktop" "$appdir/"
cp "$dist/share/icons/hicolor/scalable/apps/org.kairo.desktop.svg" "$appdir/"
ln -s usr/bin/KAIRO "$appdir/AppRun"

if [[ -z "$tool" ]]; then
    tool="$work/appimagetool"
    curl -fsSL -o "$tool" \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$tool"
fi
out="$PWD/KAIRO-${version}-x86_64.AppImage"
ARCH=x86_64 "$tool" --appimage-extract-and-run "$appdir" "$out"
echo "$out"
