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
render sounds 80

path=$(python3 -I - <<'PY'
steps = []
for i in range(60):
    x = (100 + (2 + i) * 20) / 1920
    steps.append("%.2f:%.4f,0.7222%s" % (0.2 + i * 0.1, x, ",d" if i % 4 == 1 else ""))
print(";".join(steps))
PY
)
render errors 200 LWE_HEADLESS_CURSOR_PATH="$path" LWE_FIXED_TIMESTEP=0.05

# a library puppet with a script on its layer: animation layer and bone calls, effect and blend mode changes
item=${LWE_WORKSHOP_DIR:-/workspace/SteamLibrary/steamapps/workshop/content/431960}/2997511253
if [ -f "$item/scene.pkg" ]; then
    python3 -I "$HERE/analysis/make_puppet_script_scene.py" "$item" 81 "$work/scenes/puppet" > /dev/null \
        && python3 -I "$HERE/analysis/make_puppet_script_scene.py" "${item%/*}/3435733083" 28 "$work/scenes/puppet-extras" 420,3214,3295 > /dev/null \
        && render puppet-extras 200 \
        && render puppet 150 \
        && render puppet 260 LWE_FIXED_TIMESTEP=0.05 LWE_HEADLESS_CURSOR_PATH="$(python3 -I -c '
steps = []
for i in range(60):
    steps.append("%.2f:%.3f,%.3f%s" % (0.3 + i * 0.2, 0.1 + 0.8 * (i % 10) / 9, 0.15 + 0.7 * (i // 10) / 5, ",d" if i % 3 == 1 else ""))
print(";".join(steps))')"
fi

# a video file as an image layer's texture, a script calls the video texture API (mpeg4: this sandbox has no H.264)
if command -v ffmpeg > /dev/null; then
    ffmpeg -loglevel error -y -f lavfi -i "testsrc=s=320x180:r=30" -t 1 -c:v mpeg4 "$work/clip.mp4" \
        && python3 -I "$HERE/analysis/make_video_texture_scene.py" "$work/scenes/videotexture" "$work/clip.mp4" > /dev/null \
        && render videotexture 150 \
        && python3 -I "$HERE/analysis/make_video_texture_scene.py" "$work/scenes/videotexture-runaway" "$work/clip.mp4" runaway > /dev/null \
        && render videotexture-runaway 400 LWE_SCRIPT_TIMEOUT_MS=200 LWE_FIXED_TIMESTEP=0.05
fi
