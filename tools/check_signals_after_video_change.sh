#!/usr/bin/env bash
# Checks that SIGINT and SIGTERM still reach the engine's handler after a video was replaced: libmpv resets both to
# their defaults when a player is destroyed, which made a plain kill end the engine without its exit handlers once a
# playlist had moved on. Plays a two item video sequence playlist (3 videos reload within 10 s), then reads the signal
# mask of the process and sends SIGTERM.
# Usage: tools/check_signals_after_video_change.sh <engine binary> [assets dir]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=$(readlink -f "${1:?engine binary}")
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
command -v ffmpeg > /dev/null || { echo "SKIP: no ffmpeg"; exit 0; }
work=$(mktemp -d /tmp/lwe-signals.XXXXXX)
trap 'pkill -f "[-]-window 0x0x640x360 --screenshot $work" || true; rm -rf "$work"' EXIT
steam="$work/home/.steam/steam/steamapps/common/wallpaper_engine"
mkdir -p "$steam"
ln -s "$ASSETS" "$steam/assets"
for name in a b; do
    mkdir -p "$work/$name"
    ffmpeg -loglevel error -f lavfi -i "color=c=red:s=320x180:r=30" -t 2 -c:v mpeg4 "$work/$name/v.mp4"
    echo "{\"file\":\"v.mp4\",\"title\":\"$name\",\"type\":\"video\"}" > "$work/$name/project.json"
done
printf '{"steamuser":{"general":{"playlists":[{"name":"p","items":["%s","%s"],"settings":{"mode":"timer","delay":60,"order":"sequential","videosequence":true}}]}}}\n' \
    "$work/a/project.json" "$work/b/project.json" > "$steam/config.json"

HOME="$work/home" HEADLESS_RENDER_SIZE=640x360 HEADLESS_RENDER_DELAY=100000 HEADLESS_RENDER_TIMEOUT=40 \
    "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --fps 30 --silent --playlist p \
    > "$work/log" 2>&1 < /dev/null &
sleep 12
engine=""
for pid in $(pgrep -f "[-]-window 0x0x640x360 --screenshot $work"); do
    [ "$(cat /proc/"$pid"/comm)" = linux-wallpaper ] && engine=$pid
done
[ -n "$engine" ] || { echo "FAIL: the engine is not running"; exit 1; }
loads=$(grep -c 'VO: ' "$work/log" || true)
mask=$(awk '/SigCgt/ { print $2 }' /proc/"$engine"/status)
kill -TERM "$engine"
sleep 4
if [ "$loads" -ge 3 ] && (( (16#$mask & 0x4002) == 0x4002 )) && grep -q 'Stop requested' "$work/log"; then
    echo "PASS: SIGINT and SIGTERM handled after $loads video loads"
else
    echo "FAIL: $loads loads, caught signals $mask, stop requested lines: $(grep -c 'Stop requested' "$work/log")"
    exit 1
fi
