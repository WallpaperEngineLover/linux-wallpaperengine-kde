#!/usr/bin/env bash
# The rendering options of a coverage build: bloom, HDR and 3D scenes of the library run under post processing levels,
# MSAA and SSAA, shadow and volumetric quality, canvas expansion, debug switches and odd output sizes, none of which a
# plain regression render touches.
# Usage: tools/analysis/coverage_options.sh <engine binary> [assets dir] [workshop content dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
work=$(mktemp -d /tmp/lwe-coverage-options.XXXXXX)
trap 'rm -rf "$work"' EXIT
scenes=${OPTION_SCENES:-"2321732083 2719499501 2802243144 3299228616 3777414605 3354366708 3509578940"}

# size|engine options
sets=(
    "800x450|--post-processing ultra --msaa x4"
    "800x450|--post-processing displayhdr --ssaa x2"
    "800x450|--post-processing disabled --disable-particles"
    "800x450|--volumetrics ultra --shadows ultra --msaa x8 --ssaa x3"
    "800x450|--volumetrics disabled --shadows disabled"
    "800x450|--expand-canvas --zoom 1.5 --offset 0.1,0.1 --scaling fill --clamp repeat"
    "800x450|--scaling stretch --clamp clamp --corner-color #204060 --image-filter none"
    "800x450|--fps 20 --speed 0.5 --disable-parallax --disable-mouse --disable-animations --no-shader-cache"
    "800x450|--render-debug base-only --render-debug pass-log --render-debug brightness-log"
    "800x450|--render-debug no-solid-final --render-debug no-puppet-animation"
    "450x800|--scaling fit"
    "3200x800|--scaling fill --ssaa x2"
    "320x180|--msaa x2"
)

for id in $scenes; do
    [ -d "$CONTENT/$id" ] || continue
    for entry in "${sets[@]}"; do
        size=${entry%%|*}
        options=${entry#*|}
        # shellcheck disable=SC2086
        if HEADLESS_RENDER_DELAY=40 HEADLESS_RENDER_TIMEOUT=120 HEADLESS_RENDER_SIZE=$size \
            "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --silent $options \
            "$CONTENT/$id" > "$work/run.log" 2>&1; then
            echo "ok   $id $options"
        else
            echo "FAIL $id $options: $(tail -1 "$work/run.log" | cut -c1-100)"
        fi
        rm -f "$work/shot.png"
    done
done

stats_scenes=${STATS_SCENES:-3509578940 3299228616 2321732083}
for id in $stats_scenes; do
    [ -d "$CONTENT/$id" ] || continue
    if LWE_FRAME_STATS=1 LWE_FRAME_STALL_MS=0 LWE_AUDIO_STATS=1 LWE_WEB_STATS=1 HEADLESS_RENDER_DELAY=200 \
        HEADLESS_RENDER_TIMEOUT=120 HEADLESS_RENDER_SIZE=800x450 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --silent "$CONTENT/$id" \
        > "$work/stats.log" 2>&1; then
        echo "ok   $id frame stats"
    else
        echo "FAIL $id frame stats: $(tail -1 "$work/stats.log" | cut -c1-100)"
    fi
    rm -f "$work/shot.png"
done
