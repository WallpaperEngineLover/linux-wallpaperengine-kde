#!/usr/bin/env bash
# Regression check for the grey-clouds bug: hotswapping from a scene that leaves glColorMask's alpha off (3471294034, cursor
# particles) onto a video layer wallpaper (3818199837) left the video transparent. Renders the swap and a fresh start of the
# target headlessly on the GPU (headless_render.sh) and compares the two pictures.
# Usage: tools/check_hotswap_video.sh <engine binary> [assets dir] [workshop content dir]
# Exit code 0 when the swapped picture looks like the fresh one. Needs Pillow (run with python3 -I).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
FROM=3471294034 TO=3818199837
for id in $FROM $TO; do [ -d "$CONTENT/$id" ] || { echo "SKIP: wallpaper $id not in $CONTENT"; exit 0; }; done
work=$(mktemp -d /tmp/lwe-hotswap-video.XXXXXX)
trap 'rm -rf "$work"' EXIT

echo "path=$CONTENT/$TO" > "$work/ctl"
LWE_HOTSWAP_AT_FRAME=30 HEADLESS_RENDER_DELAY=300 "$HERE/headless_render.sh" "$BINARY" "$work/swap.png" \
    --assets-dir "$ASSETS" --control-file "$work/ctl" "$CONTENT/$FROM" > "$work/swap.log" 2>&1 || { cat "$work/swap.log"; exit 1; }
HEADLESS_RENDER_DELAY=300 "$HERE/headless_render.sh" "$BINARY" "$work/fresh.png" \
    --assets-dir "$ASSETS" "$CONTENT/$TO" > "$work/fresh.log" 2>&1 || { cat "$work/fresh.log"; exit 1; }
python3 -I "$HERE/linuxvm/check_frame.py" --similar "$work/fresh.png" "$work/swap.png" 10
