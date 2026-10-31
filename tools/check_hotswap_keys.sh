#!/usr/bin/env bash
# Drives the control file's keys and the --list-* switches headlessly (headless_render.sh, LWE_HOTSWAP_AT_FRAME): every
# group of keys is written to a control file that the engine reads on frame 3, the engine has to survive it and write
# its screenshot; groups that change the picture have to change it against a run without a request. The list switches
# have to print something and exit 0.
# Usage: tools/check_hotswap_keys.sh <engine binary> [assets dir] [workshop content dir]
# Exit code 0 when every group passes. Needs Pillow (python3 -I). SCENE_ID picks another scene, 3509578940 by default.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
SCENE_ID=${SCENE_ID:-3509578940}
SCENE="$CONTENT/$SCENE_ID"
[ -d "$SCENE" ] || { echo "SKIP: wallpaper $SCENE_ID not in $CONTENT"; exit 0; }
work=$(mktemp -d /tmp/lwe-hotswap-keys.XXXXXX)
trap 'rm -rf "$work"' EXIT
failed=0

render () {
    local name=$1
    LWE_FIXED_TIMESTEP=0.1 LWE_HOTSWAP_AT_FRAME=3 HEADLESS_RENDER_DELAY=40 HEADLESS_RENDER_SIZE=800x450 \
        "$HERE/headless_render.sh" "$BINARY" "$work/$name.png" --assets-dir "$ASSETS" --silent \
        --control-file "$work/$name.ctl" "$SCENE" > "$work/$name.log" 2>&1
}

differs () {
    python3 -I - "$work/none.png" "$work/$1.png" <<'PY'
import sys
from PIL import Image, ImageChops
a, b = (Image.open(path).convert('RGB') for path in sys.argv[1:3])
bbox = ImageChops.difference(a, b).getbbox()
sys.exit(0 if bbox else 1)
PY
}

mkdir -p "$work/broken"
echo '{"file":"gone.mp4","title":"broken","type":"video"}' > "$work/broken/project.json"

: > "$work/none.ctl"
render none || { echo "FAIL: baseline did not render"; tail -5 "$work/none.log"; exit 1; }

# name|expects a different picture|keys
groups=(
    "layout|yes|scaling=fit;zoom=1.6;offset=0.2,0.1"
    "colors|yes|corner-color=#ff0000;brightness=80;contrast=70;saturation=20;hue=30;color-options=on;flip=on"
    "filter|yes|image-filter=desperado;image-filter-strength=80;color-options=on"
    "timing|no|speed=2;volume=40;fps=30;disable-animations=on;disable-parallax=on;expand-canvas=on"
    "audio|no|ambient-volume=20;audio-sensitivity=*=2;hrtf=on;audio-screen=none"
    "power|no|fps-battery=15;idle-pause=5;hwdec=no"
    "xray|no|xray=full"
    "xray-off|no|xray=disabled"
    "objects|no|layers=1;disable-object=423;enable-object=424"
    "effects|no|effects=1;disable-effect=blur;enable-effect=shake"
    "property|no|property=nothing=1;property=other=0.5"
    "alignment|no|alignment-x=25;alignment-y=75;alignment-zoom=120"
    "unknown-screen|no|screen=no-such-output;scaling=fill;zoom=2"
    "garbage|no|zoom=wide;offset=3;brightness=900;nonsense=1;speed=-4;scaling=bogus"
    "misc|no|lock-pause=off;repair-assets=on;sound-volume=*=50;transition=fade;transition-duration=300;flip=off"
    "swap-scene|no|path=$CONTENT/3299228616;layers=1;disable-object=1;property=x=1;audio-sensitivity=*=2;sound-volume=*=0.5;video-start=none"
    "swap-bad-path|no|path=/nonexistent/wallpaper"
    "swap-broken-project|no|path=$work/broken;layers=1;disable-object=1;effects=1;disable-effect=blur;property=a=1;audio-sensitivity=*=2;sound-volume=*=0.5;brightness=60;video-start=0:01"
    "live-properties|no|property=_1=2;property=_12=0;property=_11=0.9,0.1,0.1;property=_2=1,0,0"
    "live-properties-bad|no|property=_1=banana;property=_12=maybe;property=_11=zz;property=nonexistent=1"
    "hrtf-bad|no|hrtf=sometimes"
    "hrtf-off|no|hrtf=off;audio-screen=default;ambient-volume=0"
    "battery-values|no|fps-battery=none;fps-battery=10;idle-pause=0;idle-pause=off;lock-pause=on"
    "swap-screen-bad|no|screen=no-such-output;path=$CONTENT/3299228616"
    "bad-switches|no|expand-canvas=maybe;repair-assets=maybe;disable-animations=maybe;audio-sensitivity=7=abc;lock-pause=maybe"
    "switches-on|no|expand-canvas=on;repair-assets=off;disable-animations=on"
    "garbage-values|no|offset=1,x;alignment=1,2,3;alignment=a,b,c,d;zoom=nan;corner-color=zzz;image-filter-strength=abc;volume=loud;fps=0;fps-battery=x;idle-pause=x;hwdec=banana;xray=bogus;speed=abc;brightness=x;contrast=x;saturation=x;hue=x;transition=bogus;transition-duration=x;sound-volume=bad;audio-sensitivity=bad;video-start=zz;layers=maybe"
)

for group in "${groups[@]}"; do
    IFS='|' read -r name changes keys <<< "$group"
    tr ';' '\n' <<< "$keys" > "$work/$name.ctl"
    if ! render "$name"; then
        echo "FAIL: $name: the engine did not survive $keys"
        tail -5 "$work/$name.log"
        failed=1
    elif [ "$changes" = yes ] && ! differs "$name"; then
        echo "FAIL: $name: the picture did not change"
        failed=1
    else
        echo "PASS: $name"
    fi
done

LWE_FIXED_TIMESTEP=0.1 LWE_HOTSWAP_AT_FRAME=3 HEADLESS_RENDER_DELAY=40 HEADLESS_RENDER_SIZE=800x450 \
    "$HERE/headless_render.sh" "$BINARY" "$work/unreadable.png" --assets-dir "$ASSETS" --silent \
    --control-file "$work/does-not-exist.ctl" "$SCENE" > "$work/unreadable.log" 2>&1 \
    && echo "PASS: unreadable control file" || { echo "FAIL: unreadable control file"; failed=1; }

if command -v ffmpeg > /dev/null; then
    mkdir -p "$work/video"
    ffmpeg -loglevel error -f lavfi -i "color=c=green:s=320x180:r=30" -t 6 -c:v mpeg4 "$work/video/v.mp4"
    echo '{"file":"v.mp4","title":"v","type":"video"}' > "$work/video/project.json"
    videogroups=(
        "video-trim|video-start=0:01;video-end=0:04"
        "video-parts|video-segments=0:00-0:01,0:02-0:03"
        "video-seek|video-seek=0:02"
        "video-clear|video-segments=none;video-start=none;video-end=none"
        "video-bad|video-start=zz;video-end=-3;video-segments=garbage;video-seek=zz"
    )

    for group in "${videogroups[@]}"; do
        IFS='|' read -r name keys <<< "$group"
        tr ';' '\n' <<< "$keys" > "$work/$name.ctl"

        if LWE_FIXED_TIMESTEP=0.1 LWE_HOTSWAP_AT_FRAME=3 HEADLESS_RENDER_DELAY=40 HEADLESS_RENDER_SIZE=800x450 \
            "$HERE/headless_render.sh" "$BINARY" "$work/$name.png" --assets-dir "$ASSETS" --silent \
            --control-file "$work/$name.ctl" "$work/video" > "$work/$name.log" 2>&1; then
            echo "PASS: $name"
        else
            echo "FAIL: $name: the engine did not survive $keys"
            tail -5 "$work/$name.log"
            failed=1
        fi
    done
fi

# the same isolation as headless_render.sh: a private runtime dir and session bus, no desktop or audio server, no core
# dump for the host's crash handler
runtime="$work/runtime"
mkdir -m 700 "$runtime"
bus=unix:path=/nonexistent-bus
if command -v dbus-daemon > /dev/null; then
    busPid=$(dbus-daemon --session --fork --print-pid --address="unix:path=$runtime/bus")
    bus="unix:path=$runtime/bus"
    trap 'kill $busPid 2> /dev/null; rm -rf "$work"' EXIT
fi

for list in --list-properties --list-objects --list-effects --list-fonts --list-audio-objects; do
    if output=$(
        ulimit -c 0
        env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DISPLAY -u KDE_FULL_SESSION -u KDE_SESSION_VERSION \
            -u DESKTOP_SESSION -u XDG_CURRENT_DESKTOP -u XDG_SESSION_DESKTOP XDG_RUNTIME_DIR="$runtime" \
            DBUS_SESSION_BUS_ADDRESS="$bus" PULSE_SERVER=unix:/nonexistent-pulse XDG_SESSION_TYPE=headless \
            timeout 60 "$BINARY" --assets-dir "$ASSETS" "$list" "$SCENE" 2>&1
    ) && [ -n "$output" ]; then
        echo "PASS: $list"
    else
        echo "FAIL: $list printed nothing or failed"
        failed=1
    fi
done

exit $failed
