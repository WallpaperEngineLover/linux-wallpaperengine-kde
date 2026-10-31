#!/usr/bin/env bash
# Smoke check of --dump-structure (Data/Dumpers/StringPrinter): dumps a few feature-rich scenes and one video wallpaper
# headlessly and requires the dump to name every object the scene has (the "Objects count: N" header against the number
# of "Object <id>" lines) and the video's file name. A printer that crashes on a model, material, effect or sound shows
# up as a failed render.
# Usage: tools/check_dump_structure.sh <engine binary> [assets dir] [workshop content dir]
# Set SCENE_IDS (space separated) and VIDEO_ID to use other wallpapers. Missing ones are skipped.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
SCENE_IDS=${SCENE_IDS:-"3662790108 3509578940 3354366708 3521337568 3287715210 3409595232 3758232320 1251404246 2719499501"}
VIDEO_ID=${VIDEO_ID:-1367456719}
work=$(mktemp -d /tmp/lwe-dump-structure.XXXXXX)
trap 'rm -rf "$work"' EXIT
failed=0
checked=0

dump () {
    HEADLESS_RENDER_SIZE=800x450 "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --dump-structure \
        --assets-dir "$ASSETS" --fps 30 --silent "$CONTENT/$1" > "$work/$1.log" 2>&1
}

for id in $SCENE_IDS; do
    [ -d "$CONTENT/$id" ] || { echo "SKIP: $id not in $CONTENT"; continue; }
    checked=$((checked + 1))
    if ! dump "$id"; then
        echo "FAIL: $id did not render with --dump-structure"
        tail -5 "$work/$id.log"
        failed=1
        continue
    fi
    declared=$(sed -n 's/^[[:space:]]*Objects count: \([0-9]*\).*/\1/p' "$work/$id.log" | head -1)
    printed=$(grep -c '^[[:space:]]*Object [0-9-]* ' "$work/$id.log" || true)
    if [ -z "$declared" ] || [ "$declared" -eq 0 ] || [ "$declared" -ne "$printed" ]; then
        echo "FAIL: $id declares ${declared:-no} objects and prints $printed"
        failed=1
    else
        echo "PASS: $id prints its $printed objects"
    fi
done

if [ -d "$CONTENT/$VIDEO_ID" ]; then
    checked=$((checked + 1))
    if dump "$VIDEO_ID" && grep -q '^Video wallpaper: ' "$work/$VIDEO_ID.log"; then
        echo "PASS: $VIDEO_ID is dumped as a video wallpaper"
    else
        echo "FAIL: $VIDEO_ID has no \"Video wallpaper:\" line"
        failed=1
    fi
fi

[ "$checked" -gt 0 ] || { echo "SKIP: none of the wallpapers is installed"; exit 0; }
exit $failed
