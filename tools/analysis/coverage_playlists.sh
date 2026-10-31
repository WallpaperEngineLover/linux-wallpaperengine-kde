#!/usr/bin/env bash
# Playlist paths that need a playlist to advance: three short generated videos with "videosequence" on advance by
# themselves, so random and sequential order, an intro, a broken item (project that does not load, missing folder) and a
# playlist where nothing else loads all run in about 20 s each. Private HOME, the real WE config is never touched.
# Usage: tools/analysis/coverage_playlists.sh <engine binary> [assets dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=$(readlink -f "${1:?engine binary}")
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
command -v ffmpeg > /dev/null || { echo "SKIP: no ffmpeg"; exit 0; }
work=$(mktemp -d /tmp/lwe-coverage-playlists.XXXXXX)
trap 'rm -rf "$work"' EXIT
export SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/dev/null PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent

for name in a b c; do
    mkdir -p "$work/items/$name"
    case $name in a) colour=red ;; b) colour=blue ;; c) colour=green ;; esac
    ffmpeg -loglevel error -f lavfi -i "color=c=$colour:s=320x180:r=30" -t 2 -c:v mpeg4 "$work/items/$name/v.mp4"
    echo "{\"file\":\"v.mp4\",\"title\":\"$name\",\"type\":\"video\"}" > "$work/items/$name/project.json"
done
mkdir -p "$work/items/broken"
echo '{"file":"missing.mp4","title":"broken","type":"video"}' > "$work/items/broken/project.json"
steam="$work/home/.steam/steam/steamapps/common/wallpaper_engine"
mkdir -p "$steam"
ln -s "$ASSETS" "$steam/assets"
a=$work/items/a/project.json b=$work/items/b/project.json c=$work/items/c/project.json
broken=$work/items/broken/project.json gone=$work/items/gone/project.json

run () {
    local label=$1 items=$2 settings=$3
    printf '{"steamuser":{"general":{"playlists":[{"name":"p","items":[%s],"settings":{"mode":"timer","delay":60,%s}}]}}}\n' \
        "$items" "$settings" > "$steam/config.json"
    HOME="$work/home" HEADLESS_RENDER_SIZE=640x360 HEADLESS_RENDER_DELAY=100000 HEADLESS_RENDER_TIMEOUT=20 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --fps 30 --silent --playlist p \
        > "$work/$label.log" 2>&1 < /dev/null || true
    echo "$label: $(grep -c 'VO: \[libmpv\]' "$work/$label.log") loads"
}

run sequential "\"$a\",\"$b\",\"$c\"" '"order":"sequential","videosequence":true'
run random "\"$a\",\"$b\",\"$c\"" '"order":"random","videosequence":true'
run intro-random "\"$a\",\"$b\",\"$c\"" '"order":"random","videosequence":true,"beginfirst":true,"playintro":true'
run broken-item "\"$a\",\"$broken\",\"$b\"" '"order":"sequential","videosequence":true'
run missing-item "\"$a\",\"$gone\",\"$b\"" '"order":"sequential","videosequence":true'
run all-broken "\"$a\",\"$broken\",\"$gone\"" '"order":"sequential","videosequence":true'
run not-timer "\"$a\",\"$b\"" '"order":"sequential","mode":"never"'
