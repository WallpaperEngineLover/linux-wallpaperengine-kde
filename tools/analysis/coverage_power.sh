#!/usr/bin/env bash
# Battery and idle paths: a UPower mock (python-dbusmock) on a private bus that says "on battery" with --pause-on-battery
# and --fps-battery, then the same with the mains plugged in, and --idle-pause on an Xvfb display (the X11 screen saver
# extension is polled every few seconds; the pause itself needs minutes of idle time and is not reached).
# Usage: tools/analysis/coverage_power.sh <engine binary> [assets dir] [workshop content dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
SCENE=$CONTENT/${SCENE_ID:-3509578940}
[ -d "$SCENE" ] || { echo "SKIP: $SCENE missing"; exit 0; }
python3 -c "import dbusmock" 2> /dev/null || { echo "SKIP: no python-dbusmock"; exit 0; }
work=$(mktemp -d /tmp/lwe-coverage-power.XXXXXX)
bus_pid=""
mock_pid=""
trap 'kill $mock_pid $bus_pid 2> /dev/null; rm -rf "$work"' EXIT
export SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/dev/null PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent

address=$(dbus-daemon --session --fork --print-address=1 --print-pid=2 2> "$work/pid" | head -1)
bus_pid=$(head -1 "$work/pid")
DBUS_SESSION_BUS_ADDRESS=$address python3 -m dbusmock --template upower > "$work/mock.log" 2>&1 &
mock_pid=$!
sleep 3

battery () {
    DBUS_SESSION_BUS_ADDRESS=$address gdbus call --session --dest org.freedesktop.UPower --object-path /org/freedesktop/UPower \
        --method org.freedesktop.DBus.Properties.Set org.freedesktop.UPower OnBattery "<$1>" > /dev/null 2>&1
}

render () {
    local label=$1
    shift
    HEADLESS_RENDER_SYSTEM_BUS=$address HEADLESS_RENDER_DELAY=240 HEADLESS_RENDER_TIMEOUT=100 HEADLESS_RENDER_SIZE=640x360 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" --silent "$@" "$SCENE" \
        > "$work/$label.log" 2>&1
    echo "$label: exit $?, $(grep -c -E 'Paused|Resumed|battery' "$work/$label.log") power lines"
}

battery true
render on-battery --pause-on-battery --fps-battery 15
render on-battery-fps --fps-battery 15
battery false
render on-mains --pause-on-battery --fps-battery 15
HEADLESS_RENDER_SOFTWARE=1 render idle-x11 --idle-pause 1
