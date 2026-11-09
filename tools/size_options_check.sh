#!/usr/bin/env bash
# Builds the opt-in size options (-DLWE_LTO=ON -DLWE_LINK_TRIM=ON) in build-trim, runs the unit tests on that build and
# compares the sizes with the main build. The options are off in every shipped build, this keeps them from rotting.
# Run it with the other build variants (DISABLE_KDE_FEATURES, clang_build_check.sh) of a release prep.
#
# Usage: tools/size_options_check.sh [jobs]    (default 4, an LTO link is memory hungry)
# Exit code 0 when it built and the unit tests passed. CEF comes from the main build tree (hard links).
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dir=$ROOT/build-trim
jobs=${1:-4}

mkdir -p "$dir"
if [ ! -e "$dir/cef" ] && [ -d "$ROOT/build/cef" ]; then
    cp -al "$ROOT/build/cef" "$dir/cef" 2>/dev/null || cp -a "$ROOT/build/cef" "$dir/cef"
fi

cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=1 -DLWE_LTO=ON -DLWE_LINK_TRIM=ON \
    > "$dir/configure.log" 2>&1 \
    && nice cmake --build "$dir" -j "$jobs" --target linux-wallpaperengine linux-wallpaperengine-web-helper tests \
    > "$dir/build.log" 2>&1 \
    || { echo "FAIL size options: see $dir/configure.log and $dir/build.log"; grep -m5 'error:' "$dir/build.log" 2>/dev/null | cut -c1-240; exit 1; }

for file in liblinux-wallpaperengine-lib.so linux-wallpaperengine-web-helper; do
    if [ -f "$ROOT/build/output/$file" ]; then
        printf '%-40s %9d -> %9d bytes\n' "$file" "$(stat -c %s "$ROOT/build/output/$file")" "$(stat -c %s "$dir/output/$file")"
    fi
done

"$ROOT/tools/run_unit_tests.sh" --build-dir "$dir" --no-build > "$dir/unit_tests.log" 2>&1
status=$?
tail -3 "$dir/unit_tests.log"
if [ $status -eq 0 ]; then echo "PASS size options: built and unit tests passed"; else echo "FAIL size options: unit tests, see $dir/unit_tests.log"; fi
exit $status
