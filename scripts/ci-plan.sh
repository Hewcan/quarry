#!/usr/bin/env bash
# Decides how much CI a push needs (docs/SETUP.md, section "CI").
#
# Usage: ci-plan.sh <event> <base-sha> <head-sha> <forced: true|false>
#
# Prints key=value lines (the workflow appends them to $GITHUB_OUTPUT):
#   full     true  -> the Windows, macOS and Linux matrix; false -> Linux only
#   servers  true  -> Linux starts the database containers and sshd for the provider and tunnel
#                     tests; false -> those tests skip (their QUARRY_TEST_*_URL are not set)
#   matrix   the build matrix as JSON
#   reason   why, for the log
#
# The full run happens when: it was asked for (manual dispatch, "[full-ci]" in the commit message or
# PR title), the event is a pull request, the base commit is unknown, or the build, dependency or
# packaging setup changed (those can break one platform only). The servers start when full is on or
# the push touches providers, the provider API or the SSH tunnel code.
set -euo pipefail

event=${1:-push}
base=${2:-}
head=${3:-HEAD}
forced=${4:-false}

linux='{"name":"Linux","os":"ubuntu-24.04","preset":"linux-release","qt_dir":"~/Qt"}'
windows='{"name":"Windows","os":"windows-2022","preset":"windows-release","qt_dir":"C:\\Qt"}'
macos='{"name":"macOS","os":"macos-15","preset":"macos-release","qt_dir":"~/Qt"}'

emit() { # full servers reason
    local matrix
    if [ "$1" = "true" ]; then
        matrix="{\"include\":[$windows,$macos,$linux]}"
    else
        matrix="{\"include\":[$linux]}"
    fi
    echo "full=$1"
    echo "servers=$2"
    echo "matrix=$matrix"
    echo "reason=$3"
}

if [ "$forced" = "true" ]; then
    emit true true "full run requested"
    exit 0
fi
if [ "$event" = "pull_request" ]; then
    emit true true "pull request"
    exit 0
fi
if [ -z "$base" ] || [[ "$base" =~ ^0+$ ]] || ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
    emit true true "no usable base commit"
    exit 0
fi

changed=$(git diff --name-only --diff-filter=d "$base" "$head")

# Build, dependency or packaging setup: it can break one platform only.
if grep -Eq '^(CMakeLists\.txt|CMakePresets\.json|cmake/|vcpkg\.json|vcpkg-overlays/|packaging/|\.qt-version|\.github/workflows/|scripts/(setup|ci-plan)|scripts/requirements-aqt\.txt)' <<<"$changed"; then
    emit true true "build, dependency or packaging setup changed"
    exit 0
fi

if grep -Eq '^(src/providers/|tests/providers/|src/core/api/|src/core/src/(ssh|known_hosts|provider_registry)|src/core/include/quarry/(ssh|provider))' <<<"$changed"; then
    emit false true "providers or the SSH tunnel changed: Linux with the database servers"
    exit 0
fi

emit false false "Linux only, without the database servers"
