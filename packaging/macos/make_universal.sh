#!/usr/bin/env bash
# Joins the arm64 and x86_64 builds of Quarry.app into one universal bundle (H10 phase 3).
#
# Usage: make_universal.sh <arm64.app> <x64.app> <out.app>
#
# Every thin Mach-O file of the arm64 bundle is joined with its x86_64 twin by lipo. The Qt
# frameworks and plugins come from the aqt kit, which is universal already, so they are kept.
# Everything else (resources, Info.plist, symlinks) comes from the arm64 bundle.
set -euo pipefail

arm=$1
x64=$2
out=$3

rm -rf "$out"
ditto "$arm" "$out" # keeps symlinks, permissions and extended attributes

joined=0
kept=0
while IFS= read -r -d '' f; do
    rel=${f#"$out"/}
    file -b "$f" | grep -q 'Mach-O' || continue
    archs=$(lipo -archs "$f")
    case " $archs " in
        *" x86_64 "*) kept=$((kept + 1)); continue ;; # universal already
    esac
    twin="$x64/$rel"
    if [ ! -f "$twin" ]; then
        echo "no x86_64 counterpart for $rel" >&2
        exit 1
    fi
    lipo -create "$f" "$twin" -output "$f.universal"
    mv "$f.universal" "$f"
    joined=$((joined + 1))
done < <(find "$out" -type f -print0)

echo "joined $joined files with lipo, kept $kept that were universal already"
# lipo and the moves invalidated the signatures.
codesign --force --deep --sign - "$out"
codesign --verify --deep --strict "$out"
