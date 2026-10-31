#!/usr/bin/env bash
# Renders the scenes of make_script_scenes.py: script API argument checks, script errors, a runaway update and a heap
# hog (the last two with the script limits switched on, they are off by default). Audio goes nowhere.
# Usage: tools/analysis/coverage_scripts.sh <engine binary> [assets dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
work=$(mktemp -d /tmp/lwe-coverage-scripts.XXXXXX)
trap 'rm -rf "$work"' EXIT
export SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/dev/null PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent
python3 -I "$HERE/analysis/make_script_scenes.py" "$work/scenes" "$ASSETS" > /dev/null || exit 1

render () {
    local name=$1 delay=$2
    shift 2
    env "$@" HEADLESS_RENDER_DELAY="$delay" HEADLESS_RENDER_TIMEOUT=120 HEADLESS_RENDER_SIZE=800x450 \
        "$HERE/headless_render.sh" "$BINARY" "$work/$name.png" --assets-dir "$ASSETS" "$work/scenes/$name" \
        > "$work/$name.log" 2>&1
    echo "$name: exit $?, $(grep -c -i 'error' "$work/$name.log") error lines"
}

render fuzz 120
render errors 60
render runaway 90 LWE_SCRIPT_TIMEOUT_MS=200
render heap 90 LWE_SCRIPT_HEAP_MB=64 LWE_SCRIPT_BUFFER_MB=16

path=$(python3 -I - <<'PY'
steps = []
for i in range(60):
    x = (100 + (2 + i) * 20) / 1920
    steps.append("%.2f:%.4f,0.7222%s" % (0.2 + i * 0.1, x, ",d" if i % 4 == 1 else ""))
print(";".join(steps))
PY
)
render errors 200 LWE_HEADLESS_CURSOR_PATH="$path" LWE_FIXED_TIMESTEP=0.05
