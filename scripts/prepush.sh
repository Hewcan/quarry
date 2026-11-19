#!/usr/bin/env bash
# Local checks to run before `git push` (Linux and macOS; scripts/prepush.ps1 on Windows).
#
# Same order as the quick CI: clang-format 18 over the whole repository, build, ctest without
# tst_controls, and clang-tidy on the files scripts/ci-tidy-files.sh picks for the commits not yet
# on origin/main. Stops at the first failure.
#
# Usage: scripts/prepush.sh [--preset linux-debug] [--skip-tests] [--skip-tidy] [--base origin/main]
set -euo pipefail
cd "$(dirname "$0")/.."

case "$(uname -s)" in
    Darwin) preset=macos-debug ;;
    *) preset=linux-debug ;;
esac
skip_tests=false
skip_tidy=false
base=origin/main
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset=$2; shift ;;
        --skip-tests) skip_tests=true ;;
        --skip-tidy) skip_tidy=true ;;
        --base) base=$2; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n== %s\n' "$1"; }

step "clang-format (whole repository)"
git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run --Werror
echo "  formatted"

step "build ($preset)"
cmake --build --preset "$preset"

if [ "$skip_tests" = false ]; then
    step "ctest (without tst_controls)"
    ctest --preset "$preset" -j 1 --timeout 120 -E '^tst_controls$'
fi

if [ "$skip_tidy" = false ]; then
    step "clang-tidy (files changed since $base)"
    files=$(bash scripts/ci-tidy-files.sh "$base" HEAD false)
    if [ -z "$files" ]; then
        echo "  no C++ file to analyse"
    else
        echo "  $(echo "$files" | wc -l) file(s)"
        # shellcheck disable=SC2086
        clang-tidy -p "build/$preset" --quiet $files
    fi
fi

printf '\nAll local checks passed. Push once per logical step; the CI decides its level (docs/SETUP.md).\n'
