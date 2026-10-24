#!/usr/bin/env bash
# Checks the playlist settings "videosequence" (wallpaper64 2.8.42 sub_140076BE0 / sub_140067720): a video of a timer
# playlist plays once and the playlist moves on when it ends, with the setting off the 60 minute timer keeps the first
# video, and "beginfirst" + "playintro" (sub_140067A00): only the first video plays to its end, the second stays. Makes two 3 second video wallpapers and a private HOME whose config.json holds the playlist (the real WE
# config is never touched), renders headlessly and counts how often the video was (re)loaded.
# Usage: tools/check_video_sequence.sh <engine binary> [assets dir]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=$(readlink -f "${1:?engine binary}")
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
command -v ffmpeg > /dev/null || { echo "SKIP: no ffmpeg"; exit 0; }
work=$(mktemp -d /tmp/lwe-video-sequence.XXXXXX)
trap 'rm -rf "$work"' EXIT

for name in a b; do
    mkdir -p "$work/items/$name"
    colour=red; [ "$name" = b ] && colour=blue
    ffmpeg -loglevel error -f lavfi -i "color=c=$colour:s=320x180:r=30" -t 3 -pix_fmt yuv420p "$work/items/$name/v.mp4"
    echo "{\"file\":\"v.mp4\",\"title\":\"$name\",\"type\":\"video\"}" > "$work/items/$name/project.json"
done
steam="$work/home/.steam/steam/steamapps/common/wallpaper_engine"
mkdir -p "$steam"
ln -s "$ASSETS" "$steam/assets"

loads() {
    printf '{"steamuser":{"general":{"playlists":[{"name":"seq","items":["%s","%s"],"settings":{"mode":"timer","delay":60,"order":"sequential",%s}}]}}}\n' \
        "$work/items/a/project.json" "$work/items/b/project.json" "$2" > "$steam/config.json"
    HOME="$work/home" HEADLESS_RENDER_SIZE=640x360 HEADLESS_RENDER_DELAY=100000 HEADLESS_RENDER_TIMEOUT=16 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --fps 30 --silent --playlist seq \
        > "$work/run-$1.log" 2>&1 < /dev/null || true
    grep -c 'VO: \[libmpv\]' "$work/run-$1.log" || true
}

on=$(loads on '"videosequence":true')
off=$(loads off '"videosequence":false')
intro=$(loads intro '"videosequence":false,"beginfirst":true,"playintro":true')
if [ "$on" -ge 3 ] && [ "$off" -eq 1 ] && [ "$intro" -eq 2 ]; then
    echo "PASS: $on loads in 16 s with videosequence, $off without, $intro with an intro video"
else
    echo "FAIL: $on loads with videosequence (want >= 3), $off without (want 1), $intro with playintro (want 2)"
    exit 1
fi
