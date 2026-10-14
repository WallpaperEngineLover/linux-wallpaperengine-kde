#!/bin/bash
# usage: we_resize.sh <scene dir or scene.json/scene.pkg> <out dir> <WxH> [WxH ...]
# live WE 2.8.42 -playInWindow on one span_display.sh output (2560x1440), started with the first client size and
# resized to each following one with wewinsize.exe (SetWindowPos inside Wine, WE's own WM_SIZE path); grabs the
# client area as <out>/<n>_<W>x<H>.png. WE_RESIZE_FRESH=1 instead starts a new WE for every size (the reference a
# resize should match). WE_WAIT=<seconds> before the first grab (default 20), WE_RESIZE_SETTLE after a resize (3).
# RESIZE_POINTER="dx,dy" puts the pointer at that client position before each grab.
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd)
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
P=${WE_SPAN_DIR:-$S/span}
MINGW=${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || echo $HOME/.local/share/mingw-root/usr/bin/x86_64-w64-mingw32-gcc)}
bash "$HERE/span_display.sh" start 2560x1440+0+0 >/dev/null || exit 1
eval "$(bash "$HERE/span_display.sh" env)"
X=$DISPLAY WL=$WAYLAND_DISPLAY WLR=$XDG_RUNTIME_DIR
WINE=${WE_WINE_DIR:+$WE_WINE_DIR/bin/wine}; WINE=${WINE:-$(command -v wine)}
WINESERVER=${WE_WINE_DIR:+$WE_WINE_DIR/bin/wineserver}; WINESERVER=${WINESERVER:-$(command -v wineserver)}
export WINEPREFIX=${WE_PREFIX:-$P/wineprefix} DISPLAY=$X WINEDEBUG=${WE_WINEDEBUG:--all} DXVK_LOG_PATH=$P
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS; export XDG_RUNTIME_DIR=$P/run
mkdir -p $P/run && chmod 700 $P/run
export PULSE_SERVER=unix:$P/run/no-pulse PIPEWIRE_REMOTE=$P/run/no-pipewire ALSA_CONFIG_PATH=/dev/null
EXE=$P/bin/wewinsize.exe
if [ ! -f "$EXE" ] || [ "$HERE/wewinsize.c" -nt "$EXE" ]; then
    mkdir -p $P/bin && "$MINGW" -O2 -o "$EXE" "$HERE/wewinsize.c" -luser32 || exit 1
fi
F=$1; [ -d "$F" ] && F=$F/scene.json
WIN=$(echo "Z:$F" | tr '/' '\\')
OUT=$2
shift 2
mkdir -p "$OUT"
python3 - "$P/we28/config.json" <<'PY'
import json, sys
config = json.load(open(sys.argv[1]))
# WE keeps the settings under the Windows user's name
user = config[next(k for k, v in config.items() if isinstance(v, dict) and 'general' in v)]
user['general']['user']['safemode'] = False
json.dump(config, open(sys.argv[1], 'w'), indent='\t')
PY

start() {
    $WINESERVER -k 2>/dev/null; timeout 3 tail -f /dev/null
    (cd $P/we28 && nohup $WINE wallpaper64.exe -control openWallpaper -file "$WIN" -playInWindow -width ${1%x*} \
        -height ${1#*x} -x 40 -y 60 -silent > $P/we_run_last.log 2>&1 &)
    timeout ${WE_WAIT:-20} tail -f /dev/null
    DISPLAY=$X python3 - <<'PY'
from Xlib import display
d = display.Display(); root = d.screen().root
for w in root.query_tree().children:
    try:
        cls = (w.get_wm_class() or ('', ''))[1]
        g = w.get_geometry()
    except Exception:
        continue
    if cls in ('wallpaperui.exe', 'explorer.exe') or (cls == 'wallpaper64.exe' and max(g.width, g.height) < 200):
        w.unmap()
d.sync()
PY
}

grab() {
    local rect
    rect=$(cd $P && $WINE "$EXE" ${1%x*} ${1#*x} 2>/dev/null | tr -d '\r')
    set -- $rect
    # the pointer at the same client position in both engines
    [ -n "$RESIZE_POINTER" ] && DISPLAY=$X xdotool mousemove $(($1 + ${RESIZE_POINTER%,*})) $(($2 + ${RESIZE_POINTER#*,}))
    timeout ${WE_RESIZE_SETTLE:-3} tail -f /dev/null
    WAYLAND_DISPLAY=$WL XDG_RUNTIME_DIR=$WLR grim -g "$1,$2 $3x$4" "$OUT/${N}_$3x$4.png"
    echo "$N: client $3x$4 at $1,$2"
}

N=0
[ -z "$WE_RESIZE_FRESH" ] && start $1
for size in "$@"; do
    [ -n "$WE_RESIZE_FRESH" ] && start $size
    grab $size
    N=$((N + 1))
done
cp $P/we_run_last.log "$OUT/we_run.log"
$WINESERVER -k 2>/dev/null
