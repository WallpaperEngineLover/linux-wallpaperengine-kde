#!/bin/bash
# usage: ours_span.sh <scene dir> <out dir> [count] [interval seconds] [engine args...]
# this engine on the span_display.sh outputs (same layout as we_span.sh), grabs of the whole layout via grim
#   WE_SPAN_MONITORS   the outputs, as for we_span.sh
#   OURS_SPAN_SCREENS  --screen-span list (default every output: HEADLESS-1,HEADLESS-2,...); "none" gives each
#                      output its own --screen-root
#   WE_SPAN_CURSOR     "x,y x,y" pointer positions in layout pixels before each grab (a wlr virtual pointer)
#   OURS_WAIT          seconds before the first grab (default 8)
# LWE=<dir with linux-wallpaperengine> picks the build (default: this checkout's build/output)
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd)
P=${WE_SPAN_DIR:-$HOME/.local/share/we_live/span}
LWE=${LWE:-$(cd "$HERE/../../.." && pwd)/build/output}
MONITORS=${WE_SPAN_MONITORS:-1920x1080+0+0 1280x1024+2120+200}
bash "$HERE/span_display.sh" start $MONITORS >/dev/null || exit 1
eval "$(bash "$HERE/span_display.sh" env)"
SCENE=$1 OUT=$2 COUNT=${3:-3} INTERVAL=${4:-1}
shift 4 2>/dev/null || shift $#
N=$(echo $MONITORS | wc -w)
SCREENS=${OURS_SPAN_SCREENS:-$(seq -s, -f 'HEADLESS-%g' 1 $N)}
if [ "$SCREENS" = none ]; then
    TARGETS=$(for i in $(seq $N); do printf -- '--screen-root HEADLESS-%d --bg %s ' $i "$SCENE"; done)
else
    TARGETS="--screen-span $SCREENS --bg $SCENE"
fi
mkdir -p "$OUT" $P/run && chmod 700 $P/run
VP=$P/bin/wlr_virtual_pointer
D=$HERE/../../desktops
if [ -n "$WE_SPAN_CURSOR" ] && { [ ! -x $VP ] || [ $D/wlr_virtual_pointer.c -nt $VP ]; }; then
    mkdir -p $P/bin/vp
    wayland-scanner client-header $D/wlr-virtual-pointer-unstable-v1.xml $P/bin/vp/wlr-virtual-pointer-unstable-v1-client-protocol.h
    wayland-scanner private-code $D/wlr-virtual-pointer-unstable-v1.xml $P/bin/vp/wlr-virtual-pointer.c
    cc -o $VP $D/wlr_virtual_pointer.c $P/bin/vp/wlr-virtual-pointer.c -I$P/bin/vp -lwayland-client || exit 1
fi
# the session's own runtime dir (its Wayland socket), a private bus, dead audio sockets, no X display
env -i PATH=$PATH HOME=$HOME LANG=C.UTF-8 XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=$WAYLAND_DISPLAY \
    XDG_SESSION_TYPE=wayland PULSE_SERVER=unix:$P/run/no-pulse PIPEWIRE_REMOTE=$P/run/no-pipewire \
    ALSA_CONFIG_PATH=/dev/null LWE_SCRIPT_LOG=1 \
    setsid dbus-run-session -- "$LWE/linux-wallpaperengine" --silent --fps 30 \
    --assets-dir /workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets "$@" $TARGETS > "$OUT/engine.log" 2>&1 &
ENGINE=$!
timeout ${OURS_WAIT:-8} tail -f /dev/null
python3 - "$OUT" "$COUNT" "$INTERVAL" "$WE_SPAN_CURSOR" "$VP" <<'PY'
import glob, json, os, subprocess, sys, time
out, count, interval, cursor, vp = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4], sys.argv[5]
env = dict(os.environ, SWAYSOCK=glob.glob(os.environ['XDG_RUNTIME_DIR'] + '/sway-ipc.*.sock')[0])
points = [tuple(map(int, p.split(','))) for p in cursor.split()]
pointer = None
if points:
    rects = [o['rect'] for o in json.loads(subprocess.run(['swaymsg', '-t', 'get_outputs', '-r'], env=env,
                                                          capture_output=True, text=True).stdout) if o['active']]
    left, top = min(r['x'] for r in rects), min(r['y'] for r in rects)
    width = max(r['x'] + r['width'] for r in rects) - left
    height = max(r['y'] + r['height'] for r in rects) - top
    pointer = subprocess.Popen([vp], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
t0 = time.time()
for i in range(count):
    if pointer:
        x, y = points[min(i, len(points) - 1)]
        pointer.stdin.write('abs %d %d %d %d\n' % (x - left, y - top, width, height))
        pointer.stdin.flush()
        pointer.stdout.readline()
        time.sleep(1)
    subprocess.run(['grim', '%s/f%03d_%.1f.png' % (out, i, time.time() - t0)], env=env, check=True)
    time.sleep(interval)
if pointer:
    pointer.stdin.write('quit\n')
    pointer.stdin.close()
    pointer.wait(5)
PY
pkill -TERM -s $(ps -o sid= -p $ENGINE | tr -d ' ') 2>/dev/null
wait $ENGINE 2>/dev/null
