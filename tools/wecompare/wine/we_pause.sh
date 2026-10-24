#!/bin/bash
# usage: we_pause.sh <scene dir> <out dir> [seconds]   (WE_SPAN_MONITORS="1920x1080+0+0")
# live WE 2.8.42 in desktop mode on a private sway display (span_display.sh), a global pause (-control pause at
# P_AT, default 6 s) and play (PL_AT, default 12 s) while grim grabs decode the frame number blocks of the videotex
# kit's scene_pause (video texture + a script blinking square) into <out dir>/frames.txt (time, frame mod 128, square).
# -playInWindow ignores -control pause (no player objects, checked with gdb), desktop mode is needed
# the rest is we_span.sh
# live WE 2.8.42 in desktop mode on several monitors (span_display.sh), screen grabs of the whole layout via grim
#   WE_SPAN_MONITORS="1920x1080+0+0 1280x1024+2120+200"  the outputs (default these two)
#   WE_SPAN_LAYOUT=0|1|2        wallpaperconfig.layout: per monitor, stretch over all ("fit" in WE's UI), clone
#   WE_SPAN_GROUP="Monitor0,Monitor1"  a stretch group (profile.groups, layout 1) with the wallpaper on the group
#   WE_SPAN_CURSOR="x,y x,y"    pointer position in layout pixels before each grab (the last one repeats)
#   WE_WAIT=<seconds>           wait before the first grab (default 20)
# WE runs in its own install copy ($WE_SPAN_DIR/we28, config.json rewritten each run) and a Wine prefix whose
# windows are unmanaged ("Managed"="N"), so sway places WE's desktop windows where WE puts them
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd)
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
P=${WE_SPAN_DIR:-$S/span}
MONITORS=${WE_SPAN_MONITORS:-1920x1080+0+0 1280x1024+2120+200}
bash "$HERE/span_display.sh" start $MONITORS >/dev/null || exit 1
eval "$(bash "$HERE/span_display.sh" env)"
X=$DISPLAY WL=$WAYLAND_DISPLAY WLR=$XDG_RUNTIME_DIR
# WE_PROTON=<name> picks an installed Proton build (proton.sh) instead
[ -n "$WE_PROTON" ] && { eval "$(bash "$(dirname "$0")/proton.sh" env "$WE_PROTON")" || exit 1; }
WINE=${WE_WINE_DIR:+$WE_WINE_DIR/bin/wine}; WINE=${WINE:-$(command -v wine)}
WINESERVER=${WE_WINE_DIR:+$WE_WINE_DIR/bin/wineserver}; WINESERVER=${WINESERVER:-$(command -v wineserver)}
export WINEPREFIX=${WE_PREFIX:-$P/wineprefix} DISPLAY=$X WINEDEBUG=${WE_WINEDEBUG:--all} DXVK_LOG_PATH=$P
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS; export XDG_RUNTIME_DIR=$P/run
mkdir -p $P/run && chmod 700 $P/run
export PULSE_SERVER=unix:$P/run/no-pulse PIPEWIRE_REMOTE=$P/run/no-pipewire ALSA_CONFIG_PATH=/dev/null
$WINESERVER -k 2>/dev/null; timeout 3 tail -f /dev/null
F=$1; [ -d "$F" ] && F=$F/scene.json
WIN=$(echo "Z:$F" | tr '/' '\\' | tr '\\' '/')
python3 - "$P/we28/config.json" "$WIN" "${WE_SPAN_LAYOUT:-0}" "$WE_SPAN_GROUP" <<'PY'
import json, sys
path, scene, layout, group = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
config = json.load(open(path))
# WE keeps the settings under the Windows user's name
user = config[next(k for k, v in config.items() if isinstance(v, dict) and 'general' in v)]
general = user['general']
# a killed WE leaves faultyShutdown=1 in the registry, the next start then runs -nowallpapers when safemode is on
# (sub_14000A220)
general['user']['safemode'] = False
user.pop('wallpaperconfig', None)
wc = {'layout': layout, 'selectedwallpapers': {}}
if group:
    members = group.split(',')
    wc['profile'] = {'groups': {'group_span': {'monitors': members, 'layout': 1}}}
    wc['selectedwallpapers']['group_span'] = {'file': scene}
else:
    for i in range(8):
        wc['selectedwallpapers']['Monitor%d' % i] = {'file': scene}
general['wallpaperconfig'] = wc
json.dump(config, open(path, 'w'), indent='\t')
PY
cd $P/we28
: > log.txt
EXE=$S/wedesktop.exe
if [ ! -f "$EXE" ] || [ "$HERE/wedesktop.c" -nt "$EXE" ]; then
    x86_64-w64-mingw32-gcc -O2 -municode -mwindows -o "$EXE" "$HERE/wedesktop.c" -luser32 -lgdi32 || exit 1
fi
nohup $WINE "$EXE" > $P/wedesktop.log 2>&1 &
timeout 4 tail -f /dev/null
# no -control: openWallpaper rewrites wallpaperconfig without the layout, WE loads selectedwallpapers by itself
nohup $WINE wallpaper64.exe > $P/we_run_last.log 2>&1 &
timeout ${WE_WAIT:-20} tail -f /dev/null
mkdir -p "$2"
# the "Wallpaper UI" pop-up and WE's small helper windows cover the wallpaper
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
DISPLAY=$X xwininfo -root -tree > "$2/windows.txt" 2>&1
XDG_RUNTIME_DIR=$WLR SWAYSOCK=$(ls $WLR/sway-ipc.*.sock | head -1) swaymsg -t get_tree > "$2/sway_tree.json"
python3 - "$2" "${3:-24}" "$WL" "$WLR" "$WINE" <<'PY'
import glob, os, subprocess, sys, time
import numpy as np
out, dur, wl, runtime, wine = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
env = dict(os.environ, WAYLAND_DISPLAY=wl, XDG_RUNTIME_DIR=runtime, SWAYSOCK=glob.glob(runtime + '/sway-ipc.*.sock')[0])
cenv = dict(os.environ)
pts = [(20 + b * 88 + 36) * 1920 // 640 for b in range(7)]
t0 = time.time(); rows = []; ev = []
sched = [(float(os.environ.get('P_AT', 6)), 'pause'), (float(os.environ.get('PL_AT', 12)), 'play')]
procs = []
while time.time() - t0 < dur:
    t = time.time() - t0
    if sched and t >= sched[0][0]:
        _, c = sched.pop(0); ev.append((t, c))
        procs.append(subprocess.Popen([wine, 'wallpaper64.exe', '-control', c], env=cenv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=os.getcwd()))
    p = subprocess.run(['grim', '-t', 'ppm', '-'], env=env, capture_output=True).stdout
    # header P6\nW H\n255\n
    parts = p.split(b'\n', 3); w, h = map(int, parts[1].split())
    a = np.frombuffer(parts[3], np.uint8).reshape(h, w, 3).mean(2)
    bits = 0
    for px in pts:
        v = a[240 - 3:240 + 4, px - 3:px + 4].mean()
        bits = -1 if (bits < 0 or 60 < v < 190) else bits * 2 + (v >= 190)
    ind = int(a[240 - 3:240 + 4, 1870 - 3:1870 + 4].mean())
    rows.append((time.time() - t0, bits, ind, w, h))
open(out + '/frames.txt', 'w').write(''.join('%.3f %d %d\n' % r[:3] for r in rows))
open(out + '/events.txt', 'w').write(''.join('%.3f %s\n' % e for e in ev))
print('size', rows[0][3:], 'grabs', len(rows))
PY
cp $P/we28/log.txt "$2/we_log.txt"
$WINESERVER -k 2>/dev/null
