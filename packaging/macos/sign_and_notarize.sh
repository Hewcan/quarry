#!/usr/bin/env bash
# Developer ID signature and notarization of Quarry.app and its .dmg. NOT run by the CI: it needs an
# Apple Developer ID certificate and an App Store Connect notarization profile, and there are none
# yet. It is written so that adding them is a matter of secrets (docs/ARCHITECTURE.md 15), but it
# has never been run: expect to adjust the entitlements the first time.
#
# Usage: sign_and_notarize.sh <Quarry.app> <out.dmg>
#
# Environment:
#   APPLE_SIGN_IDENTITY   "Developer ID Application: Name (TEAMID)", already in the keychain
#   APPLE_NOTARY_PROFILE  a profile stored with `xcrun notarytool store-credentials`
set -euo pipefail

app=$1
dmg=$2
: "${APPLE_SIGN_IDENTITY:?APPLE_SIGN_IDENTITY is required}"
: "${APPLE_NOTARY_PROFILE:?APPLE_NOTARY_PROFILE is required}"
here=$(cd "$(dirname "$0")" && pwd)

# Hardened runtime and a timestamp are what notarization asks for. Inside out: nested code first.
while IFS= read -r -d '' f; do
    if file -b "$f" | grep -q 'Mach-O'; then
        codesign --force --options runtime --timestamp --sign "$APPLE_SIGN_IDENTITY" "$f"
    fi
done < <(find "$app/Contents/Frameworks" "$app/Contents/PlugIns" -type f -print0)
codesign --force --options runtime --timestamp --entitlements "$here/entitlements.plist" \
    --sign "$APPLE_SIGN_IDENTITY" "$app"
codesign --verify --deep --strict "$app"

"$here/make_dmg.sh" "$app" "$dmg"
codesign --force --timestamp --sign "$APPLE_SIGN_IDENTITY" "$dmg"

xcrun notarytool submit "$dmg" --keychain-profile "$APPLE_NOTARY_PROFILE" --wait
xcrun stapler staple "$dmg"
xcrun stapler validate "$dmg"
