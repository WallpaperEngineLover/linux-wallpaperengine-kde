#!/usr/bin/env bash
# Regression check for the video wallpaper start: reading the video size from mpv synchronously stalled the render thread
# ~200 ms while it set up the decoder, so the first frame sat there before playback began. Renders a video wallpaper
# headlessly on the GPU (headless_render.sh) and requires the worst gap between frames in the first LWE_FRAME_STATS window
# to stay under MAX_GAP_MS (251 ms before the fix, 51 ms after).
# Usage: tools/check_video_start.sh <engine binary> [assets dir] [workshop content dir]
# Exit code 0 when the gap is small. Set VIDEO_ID to use another video wallpaper.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
VIDEO_ID=${VIDEO_ID:-1367456719}
MAX_GAP_MS=${MAX_GAP_MS:-120}
[ -d "$CONTENT/$VIDEO_ID" ] || { echo "SKIP: wallpaper $VIDEO_ID not in $CONTENT"; exit 0; }
work=$(mktemp -d /tmp/lwe-video-start.XXXXXX)
trap 'rm -rf "$work"' EXIT

LWE_FRAME_STATS=1 HEADLESS_RENDER_DELAY=150 "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" \
    --assets-dir "$ASSETS" --fps 60 --silent "$CONTENT/$VIDEO_ID" > "$work/run.log" 2>&1 || { cat "$work/run.log"; exit 1; }
gap=$(grep -m1 '^FRAME-STATS' "$work/run.log" | sed -n 's/.*worst gap \([0-9.]*\)ms.*/\1/p')
[ -n "$gap" ] || { echo "FAIL: no FRAME-STATS line"; cat "$work/run.log"; exit 1; }
if awk -v g="$gap" -v m="$MAX_GAP_MS" 'BEGIN { exit !(g > m) }'; then
    echo "FAIL: the render thread stalled for ${gap}ms while the video started (limit ${MAX_GAP_MS}ms)"
    exit 1
fi
echo "PASS: worst frame gap while the video started ${gap}ms (limit ${MAX_GAP_MS}ms)"
