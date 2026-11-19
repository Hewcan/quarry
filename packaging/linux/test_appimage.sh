#!/usr/bin/env bash
# Checks an AppImage the way a user's machine would run it (H10 phase 4).
#
# Usage: test_appimage.sh <Quarry.AppImage> <qt-prefix>
#
# It unpacks the AppImage (no FUSE needed) and, with a bare environment (no LD_LIBRARY_PATH, no Qt
# on the PATH):
#   - every ELF file must resolve all its libraries from inside the bundle or from the system;
#   - the app starts (quarry --smoke) and sees its providers.
# The offscreen platform plugin of the Qt kit is copied in for the test only; it is not packaged.
set -euo pipefail

appimage=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
qt=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cd "$work"
"$appimage" --appimage-extract > /dev/null
root="$work/squashfs-root"

echo "== libraries"
bad=0
while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q '^ELF'; then
        missing=$(env -i PATH=/usr/bin:/bin ldd "$f" 2>/dev/null | grep 'not found' || true)
        if [ -n "$missing" ]; then
            echo "$f:"
            echo "$missing"
            bad=1
        fi
    fi
done < <(find "$root" -type f -print0)
[ "$bad" = 0 ]
echo "  every library resolves"

echo "== quarry --smoke"
mkdir -p "$work/home" "$root/usr/plugins/platforms"
cp "$qt/plugins/platforms/libqoffscreen.so" "$root/usr/plugins/platforms/"
set +e
env -i HOME="$work/home" PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen "$root/AppRun" --smoke 2>&1 | tee "$work/smoke.log"
code=${PIPESTATUS[0]}
set -e
[ "$code" = 0 ] || { echo "quarry --smoke exited with $code"; exit 1; }
grep -q "Providers available" "$work/smoke.log" || echo "::warning::the AppImage did not report its providers"
echo "ok"
