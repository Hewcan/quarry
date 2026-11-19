#!/usr/bin/env bash
# Builds Quarry.app from a build tree (H10 phase 3, docs/ARCHITECTURE.md 15).
#
# Usage: make_app.sh <build-dir> <out-dir> <version> <qt-prefix> <vcpkg-lib-dir>
#
#   <build-dir>       the CMake build tree: quarry, plugins/quarry/*.so, mariadb-plugins/,
#                     THIRD_PARTY_LICENSES.txt. Not the `cmake --install` tree: install strips the
#                     RPATHs, and without them macdeployqt cannot resolve @rpath/libqt6keychain...
#   <qt-prefix>       the Qt kit (holds bin/macdeployqt and lib/Qt*.framework), e.g. ~/Qt/6.12.0/macos
#   <vcpkg-lib-dir>   vcpkg_installed/<triplet>/lib: qtkeychain, KF6SyntaxHighlighting, unixODBC
#
# The result holds only the architecture of the machine that runs this (the aqt Qt kit is universal,
# so every framework is thinned): make_universal.sh joins the two builds. It is signed ad hoc
# (codesign -s -): Apple Silicon refuses unsigned code. A Developer ID signature and notarization
# are packaging/macos/sign_and_notarize.sh, which needs credentials.
set -euo pipefail

build=$(cd "$1" && pwd)
out=$2
version=$3
qt=$4
vlib=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
host_arch=$(uname -m) # arm64 or x86_64

mkdir -p "$out"
app="$(cd "$out" && pwd)/Quarry.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/PlugIns/quarry" "$app/Contents/PlugIns/mariadb"

cp "$build/quarry" "$app/Contents/MacOS/quarry"
cp "$build/plugins/quarry/"*.so "$app/Contents/PlugIns/quarry/"
if [ -d "$build/mariadb-plugins" ] && [ -n "$(ls -A "$build/mariadb-plugins")" ]; then
    cp "$build/mariadb-plugins/"* "$app/Contents/PlugIns/mariadb/"
fi
cp "$repo/src/app/assets/app/quarry.icns" "$app/Contents/Resources/quarry.icns"
cp "$repo/LICENSE" "$repo/NOTICE" "$app/Contents/Resources/"
if [ -f "$build/THIRD_PARTY_LICENSES.txt" ]; then cp "$build/THIRD_PARTY_LICENSES.txt" "$app/Contents/Resources/"; fi
sed "s/@VERSION@/$version/g" "$here/Info.plist.in" > "$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist"

# Qt frameworks, the platform and QML plugins and the vcpkg dylibs. The providers are listed as
# executables so that their own libraries (unixODBC...) are deployed and re-pointed too. Its own
# signature check complains about the frameworks it just modified; the final signature below
# replaces all of them, so those complaints are not failures.
args=("$app" "-qmldir=$repo/src/app/qml" "-libpath=$vlib" "-always-overwrite" "-verbose=1")
for so in "$app/Contents/PlugIns/quarry/"*.so; do args+=("-executable=$so"); done
"$qt/bin/macdeployqt" "${args[@]}"

# What the app does not use. macdeployqt copies every Quick Controls style and the SQL drivers; the
# app sets the Basic style itself and has no QtSql.
rm -rf "$app/Contents/PlugIns/sqldrivers" "$app/Contents/PlugIns/qmltooling" \
    "$app/Contents/PlugIns/tls" "$app/Contents/PlugIns/networkinformation" \
    "$app/Contents/PlugIns/iconengines" "$app/Contents/PlugIns/generic"
for style in Imagine Material Universal Fusion FluentWinUI3 Windows MacOS IOS; do
    rm -rf "$app/Contents/Frameworks/QtQuickControls2${style}"*.framework
    rm -rf "$app/Contents/Resources/qml/QtQuick/Controls/${style}"
done
rm -rf "$app/Contents/Resources/qml/QtQuick/Controls/Native" "$app/Contents/Resources/qml/QtQuick/NativeStyle"

is_macho() { file -b "$1" | grep -q 'Mach-O'; }

# The aqt kit is universal: keep only the architecture of this build (make_universal.sh joins them).
while IFS= read -r -d '' f; do
    if is_macho "$f"; then
        archs=$(lipo -archs "$f")
        if [[ "$archs" == *x86_64* && "$archs" == *arm64* ]]; then
            lipo -thin "$host_arch" "$f" -output "$f.thin"
            mv "$f.thin" "$f"
        fi
    fi
done < <(find "$app" -type f -print0)

# The absolute RPATHs of the build tree (the Qt kit, vcpkg) would let a broken bundle run on this
# machine by accident. Only @-relative ones may stay, so a missing library fails the smoke test.
strip_absolute_rpaths() {
    local file=$1 rpath
    while IFS= read -r rpath; do
        case "$rpath" in
            @* | "") ;;
            *) install_name_tool -delete_rpath "$rpath" "$file" 2>/dev/null || true ;;
        esac
    done < <(otool -l "$file" 2>/dev/null | awk '/cmd LC_RPATH/{f=1} f&&/ path /{print $2; f=0}')
}
while IFS= read -r -d '' f; do
    if is_macho "$f"; then strip_absolute_rpaths "$f"; fi
done < <(find "$app" -type f -print0)

# Ad-hoc signature over everything, after the last change to any file.
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
echo "built $app ($host_arch)"
du -sh "$app"
