#!/bin/bash
# usage: ours_resize.sh <scene dir> <out dir> <WxH> [WxH ...] [-- engine args]
# this engine as an X11 window (--window) on the span_display.sh Xwayland, started at the first size and resized to
# each following one by an X configure (xdotool windowsize), the counterpart of we_resize.sh; grabs the window as
# <out>/<n>_<W>x<H>.png. OURS_RESIZE_FRESH=1 starts a new engine per size. OURS_WAIT (default 8) before the first
# grab, OURS_RESIZE_SETTLE (default 2) after a resize.
# RESIZE_POINTER="dx,dy" puts the pointer at that client position before each grab.
# LWE=<dir with linux-wallpaperengine> picks the build (default: this checkout's build/output)
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd)
P=${WE_SPAN_DIR:-$HOME/.local/share/we_live/span}
LWE=${LWE:-$(cd "$HERE/../../.." && pwd)/build/output}
bash "$HERE/span_display.sh" start 2560x1440+0+0 >/dev/null || exit 1
eval "$(bash "$HERE/span_display.sh" env)"
SCENE=$1 OUT=$2
shift 2
SIZES=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do SIZES+=("$1"); shift; done
[ "$1" = "--" ] && shift
mkdir -p "$OUT" $P/run && chmod 700 $P/run
ENGINE=

stop_engine() {
    [ -n "$ENGINE" ] || return
    pkill -TERM -s $ENGINE 2>/dev/null
    for i in $(seq 50); do kill -0 $ENGINE 2>/dev/null || break; sleep 0.1; done
    ENGINE=
}

start() {
    local size=$1
    shift
    stop_engine
    # X11 window on the session's Xwayland: no Wayland socket, a private bus, dead audio sockets
    env -i PATH=$PATH HOME=$HOME LANG=C.UTF-8 XDG_RUNTIME_DIR=$P/run DISPLAY=$DISPLAY XDG_SESSION_TYPE=x11 \
        vblank_mode=0 PULSE_SERVER=unix:$P/run/no-pulse PIPEWIRE_REMOTE=$P/run/no-pipewire ALSA_CONFIG_PATH=/dev/null \
        setsid dbus-run-session -- "$LWE/linux-wallpaperengine" --silent --fps 30 --window 40x60x${size%x*}x${size#*x} \
        --assets-dir /workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets "$@" "$SCENE" \
        >> "$OUT/engine.log" 2>&1 &
    ENGINE=$!
    timeout ${OURS_WAIT:-8} tail -f /dev/null
}

grab() {
    local id=$(xdotool search --name '.' 2>/dev/null | while read w; do
        [ "$(xprop -id $w WM_CLASS 2>/dev/null | grep -c -i wallpaper)" = 1 ] && echo $w; done | head -1)
    [ -n "$id" ] || { echo "no engine window"; return 1; }
    xdotool windowsize $id ${1%x*} ${1#*x}
    timeout ${OURS_RESIZE_SETTLE:-2} tail -f /dev/null
    eval "$(xwininfo -id $id | awk '/Absolute upper-left X/{print "X="$4} /Absolute upper-left Y/{print "Y="$4} /Width/{print "W="$2} /Height/{print "H="$2}')"
    # the pointer at the same client position in both engines
    if [ -n "$RESIZE_POINTER" ]; then
        xdotool mousemove $((X + ${RESIZE_POINTER%,*})) $((Y + ${RESIZE_POINTER#*,}))
        timeout 1 tail -f /dev/null
    fi
    grim -g "$X,$Y ${W}x$H" "$OUT/${N}_${W}x$H.png"
    echo "$N: window ${W}x$H at $X,$Y"
}

N=0
[ -z "$OURS_RESIZE_FRESH" ] && start ${SIZES[0]} "$@"
for size in "${SIZES[@]}"; do
    [ -n "$OURS_RESIZE_FRESH" ] && start $size "$@"
    grab $size
    N=$((N + 1))
done
stop_engine
