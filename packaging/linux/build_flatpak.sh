#!/usr/bin/env bash
# Builds the local Quarry Flatpak bundle from a deployed AppDir (H10 phase 4).
#
# Usage: build_flatpak.sh <AppDir/usr> <out-dir> <version>
#
#   <AppDir/usr>  the usr tree of the AppDir that build_appimage.sh deployed
#   <out-dir>     receives Quarry-<version>.flatpak
#
# Needs flatpak and flatpak-builder, and the runtime and SDK installed (the CI step does):
#   flatpak install --user flathub org.freedesktop.Platform//25.08 org.freedesktop.Sdk//25.08
set -euo pipefail

usr=$(cd "$1" && pwd)
out=$2
version=$3
here=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$out"
out=$(cd "$out" && pwd)
stage="$out/flatpak-stage"
rm -rf "$stage" "$out/flatpak-build" "$out/flatpak-repo"
mkdir -p "$stage"
cp "$here/flatpak/dev.quarry.Quarry.yml" "$stage/"
cp -a "$usr" "$stage/appdir"

flatpak-builder --user --force-clean --repo="$out/flatpak-repo" "$out/flatpak-build" "$stage/dev.quarry.Quarry.yml"
flatpak build-bundle "$out/flatpak-repo" "$out/Quarry-$version.flatpak" dev.quarry.Quarry
echo "built $out/Quarry-$version.flatpak ($(( $(stat -c %s "$out/Quarry-$version.flatpak") / 1048576 )) MB)"
