#!/usr/bin/env bash
# Renders a wallpaper headlessly with a Tracy build (-DLWE_TRACY=ON), captures the zones and prints the busiest ones
# and the GPU time per frame. Open the saved capture with tracy-profiler for the timeline.
#
# Usage:
#   tools/tracy_profile.sh <tracy build binary> <item dir> [seconds] [engine args...]
#
# The loader zones are only recorded by a build configured with -DLWE_TRACY_ON_DEMAND=OFF, an on demand build starts
# recording when the capture connects, after the wallpaper has loaded.
#
# Needs tracy-capture and tracy-csvexport (0.14.x, the client is 0.14.1). HEADLESS_RENDER_SIZE=WxH, LWE_ASSETS_DIR and
# TRACY_OUT (capture file, default /tmp/lwe-<item>.tracy) are read from the environment.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <binary> <item dir> [seconds] [engine args...]" >&2
    exit 1
fi

BINARY="$1"
ITEM="$2"
SECONDS_TO_CAPTURE="${3:-10}"
shift 2
[ "$#" -gt 0 ] && shift

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${TRACY_OUT:-/tmp/lwe-$(basename "$ITEM").tracy}"
LOG="$(mktemp)"

# the wait for the client below takes up to 90 s and the capture starts 3 s after it, the engine has to live that long.
# Its own process group, so stopping it reaches the timeout and the engine below the script as well
HEADLESS_RENDER_KEEP_RUNNING=1 HEADLESS_RENDER_TIMEOUT=$((SECONDS_TO_CAPTURE + 130)) \
    setsid "$SCRIPT_DIR/headless_render.sh" "$BINARY" "$(mktemp -u).png" ${LWE_ASSETS_DIR:+--assets-dir "$LWE_ASSETS_DIR"} \
    --silent "$@" "$ITEM" > "$LOG" 2>&1 &
ENGINE=$!

# the client listens on 8086 once the first frame is out, scenes take a while to load
for _ in $(seq 90); do
    ss -ltn 2>/dev/null | grep -q ':8086 ' && break
    kill -0 "$ENGINE" 2>/dev/null || { echo "the engine stopped before Tracy could connect:" >&2; tail -20 "$LOG" >&2; exit 1; }
    sleep 1
done

sleep 3
timeout $((SECONDS_TO_CAPTURE + 30)) tracy-capture -o "$OUT" -a 127.0.0.1 -s "$SECONDS_TO_CAPTURE" -f > /dev/null
kill -- "-$ENGINE" 2>/dev/null || true
wait "$ENGINE" 2>/dev/null || true
rm -f "$LOG"

echo "capture: $OUT"
"$SCRIPT_DIR/tracy_summary.py" "$OUT"
