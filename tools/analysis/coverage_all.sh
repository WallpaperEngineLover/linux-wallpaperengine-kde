#!/usr/bin/env bash
# The whole coverage measurement of the engine in one go: a clean coverage build (build-cov), the unit tests, the scene
# regression render, every extra run of tools/analysis/coverage_*.sh and, with WITH_LAB=1, the desktop lab (an hour).
# Writes the gcovr summary to <out dir>/coverage-summary.json and prints the totals. Needs the workshop library and
# Wallpaper Engine's assets (LWE_WORKSHOP_DIR / LWE_ASSETS_DIR as for regress.py), nothing touches the real desktop.
# Usage: [WITH_LAB=1] tools/analysis/coverage_all.sh [out dir, default ${XDG_CACHE_HOME:-~/.cache}/lwe-analysis/coverage]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT=${1:-${XDG_CACHE_HOME:-$HOME/.cache}/lwe-analysis/coverage}
export LWE_WORKSHOP_DIR=${LWE_WORKSHOP_DIR:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
export LWE_ASSETS_DIR=${LWE_ASSETS_DIR:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
BUILD="$ROOT/build-cov"
BINARY="$BUILD/output/linux-wallpaperengine"
mkdir -p "$OUT"
cd "$ROOT" || exit 1

# -U_FORTIFY_SOURCE: the hardening block of CMakeLists.txt collides with CEF's flags under -Werror otherwise
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=1 -DCMAKE_CXX_FLAGS="--coverage -U_FORTIFY_SOURCE" \
    -DCMAKE_C_FLAGS="--coverage" -DCMAKE_CXX_FLAGS_RELEASE=-O1 -DCMAKE_C_FLAGS_RELEASE=-O1 > "$OUT/cmake.log" 2>&1 || exit 1
find "$BUILD" -name '*.gcda' -delete
make -C "$BUILD" -j"$(nproc)" linux-wallpaperengine linux-wallpaperengine-lib linux-wallpaperengine-web-helper tests \
    > "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; exit 1; }

step () {
    local label=$1
    shift
    echo "== $label"
    "$@" > "$OUT/$label.log" 2>&1 || echo "   (exit $?, see $OUT/$label.log)"
}

step unit-tests "$BUILD/output/tests"
step regress python3 "$ROOT/tools/regression/regress.py" run --gpu --jobs 4 "$BINARY" "$OUT/regress"
step extras "$HERE/coverage_extras.sh" "$BINARY"
step testscenes "$HERE/coverage_testscenes.sh" "$BINARY"
step audio "$HERE/coverage_audio.sh" "$BINARY"
step media "$HERE/coverage_media.sh" "$BINARY"
step options "$HERE/coverage_options.sh" "$BINARY"
step playlists "$HERE/coverage_playlists.sh" "$BINARY"
step scripts "$HERE/coverage_scripts.sh" "$BINARY"
step shortcuts "$HERE/coverage_shortcuts.sh" "$BINARY"
step cli "$HERE/coverage_cli.sh" "$BINARY"
step power "$HERE/coverage_power.sh" "$BINARY"
step hotswap-keys "$ROOT/tools/check_hotswap_keys.sh" "$BINARY"
step signals "$ROOT/tools/check_signals_after_video_change.sh" "$BINARY"
if [ -n "${WITH_LAB:-}" ]; then
    echo "== desktop lab"
    LWE_BINARY="$BINARY" python3 -m pytest tests/desktops -q -p no:cacheprovider > "$OUT/lab.log" 2>&1 || echo "   (lab exit $?)"
fi

gcovr -r . --object-directory "$BUILD" --filter 'src/WallpaperEngine/' --exclude 'src/WallpaperEngine/Testing' -j "$(nproc)" \
    --gcov-ignore-parse-errors suspicious_hits.warn_once_per_file --json-summary "$OUT/coverage-summary.json" > /dev/null 2>&1
python3 -I - "$OUT/coverage-summary.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
print("lines %d of %d: %.1f %%" % (data["line_covered"], data["line_total"], data["line_percent"]))
# JS.cpp (1024 template instantiations) and Log.h (templates per translation unit) are counted many times over
skip = ("Scripting/JS.cpp", "Logging/Log.h")
files = [f for f in data["files"] if not f["filename"].endswith(skip)]
total = sum(f["line_total"] for f in files)
covered = sum(f["line_covered"] for f in files)
print("without JS.cpp and Log.h: %.1f %%" % (100.0 * covered / total))
PY
