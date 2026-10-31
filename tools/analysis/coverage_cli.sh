#!/usr/bin/env bash
# Command line paths that no scene render reaches: screenshot formats (and one that cannot be written), wallpapers that do
# not load (missing folder, no project.json, broken JSON, presets with bad or missing dependencies) and the refusals of
# the hidden --web-host mode. Nothing here needs a window beyond the headless driver.
# Usage: tools/analysis/coverage_cli.sh <engine binary> [assets dir] [workshop content dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=$(readlink -f "${1:?engine binary}")
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
work=$(mktemp -d /tmp/lwe-coverage-cli.XXXXXX)
trap 'rm -rf "$work"' EXIT
export SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/dev/null PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent
SCENE=$CONTENT/${SCENE_ID:-3509578940}
[ -d "$SCENE" ] || { echo "SKIP: $SCENE missing"; exit 0; }

render () {
    local label=$1 output=$2
    shift 2
    HEADLESS_RENDER_DELAY=20 HEADLESS_RENDER_TIMEOUT=60 HEADLESS_RENDER_SIZE=640x360 \
        "$HERE/headless_render.sh" "$BINARY" "$output" --assets-dir "$ASSETS" --silent "$@" > "$work/$label.log" 2>&1
    echo "$label: exit $?"
}

for extension in bmp jpg jpeg png tga; do
    render "screenshot-$extension" "$work/shot.$extension" "$SCENE"
done
render screenshot-unwritable /proc/lwe-no/shot.png "$SCENE"

mkdir -p "$work/empty" "$work/broken" "$work/store/preset-a" "$work/store/preset-b" "$work/store/preset-c" "$work/store/preset-d"
echo '{ "file": ' > "$work/broken/project.json"
echo '{ "preset": {}, "dependency": ".." }' > "$work/store/preset-a/project.json"
echo '{ "preset": {}, "dependency": "a/b" }' > "$work/store/preset-b/project.json"
echo '{ "preset": {}, "dependency": "99999999" }' > "$work/store/preset-c/project.json"
echo '{ "preset": {}, "dependency": "" }' > "$work/store/preset-d/project.json"
for target in "$work/does-not-exist" "$work/empty" "$work/broken" "$work/store/preset-a" "$work/store/preset-b" \
    "$work/store/preset-c" "$work/store/preset-d"; do
    render "bad-$(basename "$target")" "$work/bad.png" "$target"
done

host () {
    ulimit -c 0
    local label=$1
    shift
    timeout 60 "$BINARY" --assets-dir "$ASSETS" --web-host "$@" > "$work/$label.log" 2>&1
    echo "$label: exit $?"
}
# only the argument check: a --web-host that gets as far as CEF without the display setup of a real host aborts with a
# trap, and the host's core_pattern would send that to the desktop's crash handler
host web-host-frames --web-host-frames bogus --web-host-shm lwe-none "$SCENE"
