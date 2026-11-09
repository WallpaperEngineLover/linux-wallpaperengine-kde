#!/usr/bin/env bash
# Renders every test scene headlessly with a coverage build, one at a time, for the features the workshop library does
# not use: the generated scenes under a tests dir (default ~/.local/share/we_live/tests, made for the comparisons against
# live Wallpaper Engine: sound, skinned models, ropes, particle trees, shadows, video textures...; the particle
# operator scenes come from tools/analysis/make_particle_scenes.py and the light kind variants from make_light_scenes.py, both made here) and the preview
# scenes Wallpaper Engine ships in its assets (effects/*/preview, presets, scenes).
# Audio goes to SDL's disk driver and a dead PulseAudio server, never to a real device.
# Usage: tools/analysis/coverage_testscenes.sh <engine binary> [tests dir] [assets dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=${1:?engine binary}
TESTS=${2:-$HOME/.local/share/we_live/tests}
ASSETS=${3:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
python3 -I "$HERE/analysis/make_particle_scenes.py" "$TESTS/particles"
[ -d "$TESTS/volshadow/point" ] && python3 -I "$HERE/analysis/make_light_scenes.py" "$TESTS/volshadow/point" "$TESTS/lights"
[ -d "$TESTS/texanim/probe" ] && python3 -I "$HERE/analysis/make_texanim_reverse.py" "$TESTS/texanim/probe" "$TESTS/texanim-reverse"
work=$(mktemp -d /tmp/lwe-coverage-testscenes.XXXXXX)
trap 'rm -rf "$work"' EXIT
export SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/dev/null PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent
ok=0
bad=0

while read -r project; do
    folder=$(dirname "$project")
    if HEADLESS_RENDER_DELAY=60 HEADLESS_RENDER_TIMEOUT=90 HEADLESS_RENDER_SIZE=800x450 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" "$folder" > "$work/run.log" 2>&1; then
        ok=$((ok + 1))
    else
        bad=$((bad + 1))
        echo "FAIL ${folder#"$TESTS"/}: $(tail -1 "$work/run.log" | cut -c1-120)"
    fi
    rm -f "$work/shot.png"
done < <(find "$TESTS" "$ASSETS/effects" "$ASSETS/presets" "$ASSETS/scenes" -name project.json 2>/dev/null | sort)

echo "rendered $ok, failed $bad"
