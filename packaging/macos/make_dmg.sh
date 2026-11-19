#!/usr/bin/env bash
# Packs Quarry.app into a compressed .dmg with a link to /Applications (H10 phase 3).
# ULMO is LZMA compression (macOS 10.15 and later), much smaller than UDZO for a Qt bundle.
#
# Usage: make_dmg.sh <Quarry.app> <out.dmg>
set -euo pipefail

app=$1
out=$2
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

ditto "$app" "$stage/Quarry.app"
ln -s /Applications "$stage/Applications"
rm -f "$out"
hdiutil create -volname "Quarry" -srcfolder "$stage" -ov -format ULMO "$out"
echo "built $out"
