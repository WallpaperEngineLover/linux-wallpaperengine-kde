#!/usr/bin/env bash
# The media paths of a coverage build: scenes with media scripts run for a while on a session bus that has a fake MPRIS
# player (tools/desktops/fake_mpris_player.py: track changes, cover art as a file, an unreachable address and none, play
# states, position), through headless_render.sh's HEADLESS_RENDER_BUS_HELPER.
# Usage: tools/analysis/coverage_media.sh <engine binary> [assets dir] [workshop content dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
work=$(mktemp -d /tmp/lwe-coverage-media.XXXXXX)
trap 'rm -rf "$work"' EXIT
scenes=${MEDIA_SCENES:-"2250930375 2901582518 2902406982 2938612768 3457765188"}

for id in $scenes; do
    [ -d "$CONTENT/$id" ] || continue
    if HEADLESS_RENDER_BUS_HELPER="python3 -I $HERE/desktops/fake_mpris_player.py" HEADLESS_RENDER_DELAY=900 \
        HEADLESS_RENDER_TIMEOUT=120 HEADLESS_RENDER_SIZE=800x450 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --silent "$CONTENT/$id" \
        > "$work/$id.log" 2>&1; then
        echo "ok   $id"
    else
        echo "FAIL $id"
        tail -3 "$work/$id.log"
    fi
    rm -f "$work/shot.png"
done

id=${CHURN_SCENE:-2901582518}
if [ -d "$CONTENT/$id" ]; then
    if LWE_FAKE_MPRIS_CHURN=1 HEADLESS_RENDER_BUS_HELPER="python3 -I $HERE/desktops/fake_mpris_player.py" \
        HEADLESS_RENDER_DELAY=1800 HEADLESS_RENDER_TIMEOUT=120 HEADLESS_RENDER_SIZE=800x450 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --silent "$CONTENT/$id" \
        > "$work/churn.log" 2>&1; then
        echo "ok   churn $id"
    else
        echo "FAIL churn $id"
        tail -3 "$work/churn.log"
    fi
fi
