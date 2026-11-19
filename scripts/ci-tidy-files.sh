#!/usr/bin/env bash
# Prints, one per line, the C++ files clang-tidy must analyse in CI.
#
# Usage: ci-tidy-files.sh <base-sha> <head-sha> <full: true|false>
#
# Only the files changed between base and head are analysed (plus the .cpp files that include a
# changed header). Everything is analysed instead when:
#   - a full run was asked for (manual dispatch, "[full-ci]" in the message);
#   - the base is unknown (new branch, force push);
#   - the build or analysis setup changed (root CMake files, presets, vcpkg, .clang-tidy, the Qt
#     version; not the workflow, which cannot change what clang-tidy sees);
#   - more than kMaxChangedSources header/source files changed (a big change or new development).
set -euo pipefail

base=${1:-}
head=${2:-HEAD}
full=${3:-false}
kMaxChangedSources=40

all_files() {
    git ls-files 'src/*.cpp' 'tests/*.cpp'
}

full_run() {
    echo "clang-tidy: full run ($1)" >&2
    all_files
    exit 0
}

[ "$full" = "true" ] && full_run "requested"
if [ -z "$base" ] || [[ "$base" =~ ^0+$ ]] || ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
    full_run "no usable base commit"
fi

changed=$(git diff --name-only --diff-filter=d "$base" "$head")

if grep -Eq '^(CMakeLists\.txt|CMakePresets\.json|cmake/|vcpkg\.json|vcpkg-overlays/|\.clang-tidy|\.qt-version|scripts/ci-tidy-files\.sh)' <<<"$changed"; then
    full_run "build or analysis setup changed"
fi

sources=$(grep -E '\.(h|cpp)$' <<<"$changed" || true)
count=0
[ -n "$sources" ] && count=$(wc -l <<<"$sources")
if [ "$count" -gt "$kMaxChangedSources" ]; then
    full_run "$count changed sources"
fi

{
    # Changed .cpp files under src/ and tests/ (a deleted file is already filtered out).
    grep -E '^(src|tests)/.*\.cpp$' <<<"$sources" || true
    # A changed header: every .cpp that mentions its file name.
    for header in $(grep -E '\.h$' <<<"$sources" || true); do
        name=$(basename "$header")
        all_files | while read -r cpp; do
            grep -q -F "$name" "$cpp" && echo "$cpp"
        done
    done
} | sort -u | while read -r file; do
    [ -n "$file" ] && [ -f "$file" ] && echo "$file"
done || true

echo "clang-tidy: diff run ($count changed sources)" >&2
