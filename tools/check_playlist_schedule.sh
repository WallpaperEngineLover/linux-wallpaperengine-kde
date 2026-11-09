#!/usr/bin/env bash
# Daytime and day of week playlists pick the wallpaper by the local clock. A daytime playlist whose first item ends at
# the next full minute must start on item 0 and switch to item 1 about a minute later, a day of week playlist of two
# items must start on the item its weekday gives (three days for the first item, Sunday first) and stay there.
# Private HOME, the real WE config is never touched. Takes about 80 s.
# Usage: tools/check_playlist_schedule.sh <engine binary> [assets dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=$(readlink -f "${1:?engine binary}")
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
command -v ffmpeg > /dev/null || { echo "SKIP: no ffmpeg"; exit 0; }
work=$(mktemp -d /tmp/lwe-check-schedule.XXXXXX)
trap 'rm -rf "$work"' EXIT
export SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/dev/null PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent

for name in a b; do
    mkdir -p "$work/items/$name"
    case $name in a) colour=red ;; b) colour=blue ;; esac
    ffmpeg -loglevel error -f lavfi -i "color=c=$colour:s=320x180:r=30" -t 5 -c:v mpeg4 "$work/items/$name/v.mp4"
    echo "{\"file\":\"v.mp4\",\"title\":\"$name\",\"type\":\"video\"}" > "$work/items/$name/project.json"
done
steam="$work/home/.steam/steam/steamapps/common/wallpaper_engine"
mkdir -p "$steam"
ln -s "$ASSETS" "$steam/assets"

run () {
    local label=$1 timeout=$2
    LC_ALL=C HOME="$work/home" HEADLESS_RENDER_SIZE=640x360 HEADLESS_RENDER_DELAY=100000 HEADLESS_RENDER_TIMEOUT=$timeout \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --fps 30 --silent --playlist p \
        > "$work/$label.log" 2>&1 < /dev/null || true
}

fail=0
check () {
    if [ "$2" = "$3" ]; then echo "ok: $1"; else echo "FAIL: $1 (got '$2', wanted '$3')"; fail=1; fi
}

while [ "$(date +%S)" -gt 25 ] || [ "$(date +%H%M)" = 2359 ]; do sleep 1; done
minutes=$((10#$(date +%H) * 60 + 10#$(date +%M)))
end=$(awk -v m=$((minutes + 1)) 'BEGIN { printf "%.12f", m / 1440 }')
printf '{"steamuser":{"general":{"playlists":[{"name":"p","items":[{"file":"%s","daytimeend":%s},"%s"],"settings":{"mode":"daytime"}}]}}}\n' \
    "$work/items/a/project.json" "$end" "$work/items/b/project.json" > "$steam/config.json"
run daytime 75
check "daytime starts on item 0" "$(grep -c 'daytime starts with item 0' "$work/daytime.log")" 1
check "daytime switches to item 1" "$(grep -c 'daytime switches to item 1' "$work/daytime.log")" 1

printf '{"steamuser":{"general":{"playlists":[{"name":"p","items":["%s","%s"],"settings":{"mode":"dayofweek"}}]}}}\n' \
    "$work/items/a/project.json" "$work/items/b/project.json" > "$steam/config.json"
run dayofweek 12
wday=$(date +%w)
want=1
[ "$wday" -le 2 ] && want=0
check "day of week starts on item $want" "$(grep -c "dayofweek starts with item $want" "$work/dayofweek.log")" 1
check "day of week does not switch" "$(grep -c 'dayofweek switches' "$work/dayofweek.log")" 0

printf '{"steamuser":{"general":{"playlists":[{"name":"p","items":["%s","%s"],"settings":{"mode":"never"}}]}}}\n' \
    "$work/items/a/project.json" "$work/items/b/project.json" > "$steam/config.json"
run never 12
check "never does not switch" "$(grep -c 'switches to item' "$work/never.log")" 0
exit $fail
