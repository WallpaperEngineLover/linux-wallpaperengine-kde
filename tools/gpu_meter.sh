#!/bin/bash
# Measures how much of the GPU (and CPU) one wallpaper takes, from the engine's own amdgpu/i915/xe per-client
# counters (/proc/<pid>/fdinfo drm-engine-gfx/render), so other programs on the same GPU don't skew it.
# Renders headlessly through headless_render.sh, waits WARMUP seconds, then samples for SECONDS.
#
# Usage: tools/gpu_meter.sh <engine binary> <workshop item dir> [WxH] [engine args...]
# Env: WARMUP (default 15), SECONDS_SAMPLED (default 15), LWE_ASSETS_DIR. Don't set LWE_FRAME_STATS, its glFinish
# and timer queries roughly triple the numbers.
set -euo pipefail

BINARY=$(readlink -f "$1")
ITEM=$2
shift 2
SIZE=3840x2160
if [[ ${1:-} =~ ^[0-9]+x[0-9]+$ ]]; then
    SIZE=$1
    shift
fi
WARMUP=${WARMUP:-15}
SAMPLED=${SECONDS_SAMPLED:-15}
OUT=$(mktemp -d)
RUNNER=
trap '[ -n "$RUNNER" ] && kill "$RUNNER" 2>/dev/null; rm -rf "$OUT"' EXIT

ulimit -c 0
HEADLESS_RENDER_KEEP_RUNNING=1 HEADLESS_RENDER_SIZE=$SIZE HEADLESS_RENDER_DELAY=5000 \
    HEADLESS_RENDER_TIMEOUT=$((WARMUP + SAMPLED + 10)) \
    "$(dirname "$0")/headless_render.sh" "$BINARY" "$OUT/shot.png" \
    ${LWE_ASSETS_DIR:+--assets-dir "$LWE_ASSETS_DIR"} "$@" "$ITEM" > "$OUT/log" 2>&1 &
RUNNER=$!
sleep "$WARMUP"

PID=$(pgrep -f "^$BINARY --window 0x0x$SIZE" | head -1 || true)
if [ -z "$PID" ]; then
    echo "engine did not start, log:" >&2
    cat "$OUT/log" >&2
    exit 1
fi

gpu_ns () {
    # one fd per DRM client can be listed several times, count each client id once
    cat /proc/"$PID"/fdinfo/* 2>/dev/null |
        awk '/^drm-client-id/ { c = $2 } /^drm-engine-(gfx|render):/ { if (!(c in s)) { s[c] = 1; t += $2 } } END { print t + 0 }'
}
cpu_ticks () { awk '{ print $14 + $15 }' /proc/"$PID"/stat; }

g0=$(gpu_ns); c0=$(cpu_ticks); t0=$(date +%s.%N)
sleep "$SAMPLED"
g1=$(gpu_ns); c1=$(cpu_ticks); t1=$(date +%s.%N)

awk -v g="$((g1 - g0))" -v c="$((c1 - c0))" -v t0="$t0" -v t1="$t1" -v hz="$(getconf CLK_TCK)" \
    'BEGIN { dt = t1 - t0; printf "gpu %.1f%%  cpu %.1f%% of one core\n", g / 1e9 / dt * 100, c / hz / dt * 100 }'

kill "$RUNNER" 2>/dev/null || true
wait "$RUNNER" 2>/dev/null || true
