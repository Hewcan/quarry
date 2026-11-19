#!/usr/bin/env bash
# Builds the Quarry AppImage from a build tree (H10 phase 4, docs/ARCHITECTURE.md 15).
#
# Usage: build_appimage.sh <build-dir> <out-dir> <version> <qt-prefix> <vcpkg-lib-dir>
#
#   <build-dir>       the CMake build tree of a Release build (cmake --install runs on it)
#   <out-dir>         receives AppDir/ and Quarry-<version>-x86_64.AppImage
#   <qt-prefix>       the Qt kit (holds bin/qmake and lib/libQt6*.so), e.g. ~/Qt/6.12.0/gcc_64
#   <vcpkg-lib-dir>   vcpkg_installed/<triplet>/lib: qtkeychain, KF6SyntaxHighlighting, unixODBC
#
# The tools are official AppImage binaries downloaded here, pinned by version and checked against
# their SHA-256 before they run: a changed download breaks the build instead of slipping in. They are
# used only to build the package; nothing of them goes into it. Built on Ubuntu 24.04, the package
# needs a glibc as new as that one.
set -euo pipefail

build=$(cd "$1" && pwd)
out=$2
version=$3
qt=$4
vlib=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)

LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
PLUGIN_QT_URL=https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage
PLUGIN_QT_SHA256=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

mkdir -p "$out"
out=$(cd "$out" && pwd)
tools="$out/tools"
mkdir -p "$tools"

fetch() { # name url sha256
    local name=$1 url=$2 sha=$3
    curl -fsSL --retry 3 -o "$tools/$name" "$url"
    echo "$sha  $tools/$name" | sha256sum -c -
    chmod +x "$tools/$name"
}
fetch linuxdeploy "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA256"
fetch linuxdeploy-plugin-qt "$PLUGIN_QT_URL" "$PLUGIN_QT_SHA256"
fetch appimagetool "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256"
export PATH="$tools:$PATH"
# The runners have no FUSE: AppImages run by unpacking themselves.
export APPIMAGE_EXTRACT_AND_RUN=1

appdir="$out/AppDir"
rm -rf "$appdir"
cmake --install "$build" --prefix "$appdir/usr"

install -Dm644 "$here/dev.quarry.Quarry.desktop" "$appdir/usr/share/applications/dev.quarry.Quarry.desktop"
mkdir -p "$appdir/usr/share/metainfo"
sed -e "s/@VERSION@/$version/g" -e "s/@DATE@/$(date -u +%F)/g" "$here/dev.quarry.Quarry.metainfo.xml.in" \
    > "$appdir/usr/share/metainfo/dev.quarry.Quarry.metainfo.xml"
install -Dm644 "$repo/src/app/assets/app/quarry-256.png" "$appdir/usr/share/icons/hicolor/256x256/apps/dev.quarry.Quarry.png"
install -Dm644 "$repo/src/app/assets/app/quarry-512.png" "$appdir/usr/share/icons/hicolor/512x512/apps/dev.quarry.Quarry.png"

# Qt libraries, platform and QML plugins and the vcpkg libraries (qtkeychain, KF6SyntaxHighlighting,
# unixODBC, libsecret...). The installed binaries lost their build RPATHs, so the loader gets the
# directories of the Qt kit and of vcpkg while linuxdeploy resolves them with ldd. The providers are
# loaded by the app, not linked to it: --deploy-deps-only takes their libraries without moving them.
export QMAKE="$qt/bin/qmake"
export QML_SOURCES_PATHS="$repo/src/app/qml"
export LD_LIBRARY_PATH="$qt/lib:$vlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LINUXDEPLOY_OUTPUT_VERSION="$version"
linuxdeploy --appdir "$appdir" \
    --executable "$appdir/usr/bin/quarry" \
    --deploy-deps-only "$appdir/usr/bin/plugins/quarry" \
    --desktop-file "$appdir/usr/share/applications/dev.quarry.Quarry.desktop" \
    --icon-file "$repo/src/app/assets/app/quarry-512.png" --icon-filename dev.quarry.Quarry \
    --custom-apprun "$here/AppRun" \
    --plugin qt

# What the app does not use: every Quick Controls style but Basic (the app sets it itself), the SQL
# drivers (no QtSql), debugging, TLS, network information, icon engines and generic input plugins.
rm -rf "$appdir/usr/plugins/sqldrivers" "$appdir/usr/plugins/qmltooling" "$appdir/usr/plugins/tls" \
    "$appdir/usr/plugins/networkinformation" "$appdir/usr/plugins/iconengines" "$appdir/usr/plugins/generic"
for style in Imagine Material Universal Fusion FluentWinUI3; do
    rm -rf "$appdir/usr/qml/QtQuick/Controls/$style"
    rm -f "$appdir/usr/lib/libQt6QuickControls2${style}"*.so*
done
# The native style links the Fusion one, which is gone: both go.
rm -rf "$appdir/usr/qml/QtQuick/Controls/Native" "$appdir/usr/qml/QtQuick/NativeStyle"
rm -f "$appdir/usr/lib/libQt6QuickControls2Native"*.so*

appimage="$out/Quarry-$version-x86_64.AppImage"
rm -f "$appimage"
ARCH=x86_64 appimagetool --no-appstream "$appdir" "$appimage"
echo "built $appimage ($(( $(stat -c %s "$appimage") / 1048576 )) MB)"
