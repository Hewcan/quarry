#!/usr/bin/env bash
# Builds Quarry.app from the tree `cmake --install` laid out (H10 phase 3, docs/ARCHITECTURE.md 15).
#
# Usage: make_app.sh <install-prefix> <out-dir> <version> <qt-prefix> <vcpkg-lib-dir>
#
#   <install-prefix>  bin/quarry, bin/plugins/quarry/*.so, bin/mariadb-plugins/, LICENSE, NOTICE...
#   <qt-prefix>       the Qt kit (holds bin/macdeployqt and lib/Qt*.framework), e.g. ~/Qt/6.12.0/macos
#   <vcpkg-lib-dir>   vcpkg_installed/<triplet>/lib: qtkeychain, KF6SyntaxHighlighting, unixODBC
#
# The bundle is signed ad hoc (codesign -s -): Apple Silicon refuses unsigned code. A Developer ID
# signature and notarization are packaging/macos/sign_and_notarize.sh, which needs credentials.
set -euo pipefail

prefix=$(cd "$1" && pwd)
out=$2
version=$3
qt=$4
vlib=$5
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)

mkdir -p "$out"
app="$(cd "$out" && pwd)/Quarry.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/PlugIns/quarry" "$app/Contents/PlugIns/mariadb"

cp "$prefix/bin/quarry" "$app/Contents/MacOS/quarry"
cp "$prefix/bin/plugins/quarry/"* "$app/Contents/PlugIns/quarry/"
if [ -d "$prefix/bin/mariadb-plugins" ] && [ -n "$(ls -A "$prefix/bin/mariadb-plugins")" ]; then
    cp "$prefix/bin/mariadb-plugins/"* "$app/Contents/PlugIns/mariadb/"
fi
cp "$repo/src/app/assets/app/quarry.icns" "$app/Contents/Resources/quarry.icns"
for f in LICENSE NOTICE THIRD_PARTY_LICENSES.txt; do
    if [ -f "$prefix/$f" ]; then cp "$prefix/$f" "$app/Contents/Resources/$f"; fi
done
sed "s/@VERSION@/$version/g" "$here/Info.plist.in" > "$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist"

# Qt frameworks, the platform and QML plugins and the vcpkg dylibs. The providers are listed as
# executables so that their own libraries (unixODBC...) are deployed and re-pointed too.
args=("$app" "-qmldir=$repo/src/app/qml" "-libpath=$vlib" "-always-overwrite" "-verbose=1")
for so in "$app/Contents/PlugIns/quarry/"*.so; do args+=("-executable=$so"); done
"$qt/bin/macdeployqt" "${args[@]}"

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
    if file -b "$f" | grep -q 'Mach-O'; then strip_absolute_rpaths "$f"; fi
done < <(find "$app" -type f -print0)

# Ad-hoc signature over everything, after the last change to any file.
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
echo "built $app"
