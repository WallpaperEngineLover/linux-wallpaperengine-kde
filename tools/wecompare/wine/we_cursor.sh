#!/bin/bash
# usage: we_cursor.sh <scene dir or scene.json/scene.pkg> <out dir> "x,y x,y,d x,y,u x,y,c ..." [settle seconds]
# like we_full.sh, but warps the pointer (screen px on :98) before each grab: f<i>_<x>_<y>.png.
# d presses, u releases, c clicks the left button there
ulimit -c 0
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
D=:98
bash "$(dirname "$0")/we_display.sh" >/dev/null || exit 1
export WINEPREFIX=$S/wineprefix DISPLAY=$D WINEDEBUG=-all DXVK_LOG_PATH=$S
# Wine must only see the isolated :98, not the desktop session (wayland-0, D-Bus) in the shared runtime dir
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS; export XDG_RUNTIME_DIR=$S/run
# keep Wine off the desktop's audio server (a shared /run/user/<uid>/pulse would play the wallpaper's sounds on real speakers and feed real system audio to the visualizers)
export PULSE_SERVER=unix:$S/run/no-pulse PIPEWIRE_REMOTE=$S/run/no-pipewire ALSA_CONFIG_PATH=/dev/null
/usr/lib/wine/wineserver -k 2>/dev/null; timeout 4 tail -f /dev/null
cd $S/we28
F=$1; [ -d "$F" ] && F=$F/scene.json
WIN=$(echo "Z:$F" | tr '/' '\\')
nohup /usr/lib/wine/wine64 wallpaper64.exe -control openWallpaper -file "$WIN" -monitor 0 -playInWindow -silent > $S/we_run_last.log 2>&1 &
timeout 25 tail -f /dev/null
mkdir -p "$2"
python3 - "$2" "$3" "${4:-3}" "$D" <<'PY'
import sys, time, mss
from PIL import Image
from Xlib import display
from Xlib.ext import xtest
d=display.Display(sys.argv[4]); root=d.screen().root
def walk(w):
    for c in w.query_tree().children:
        yield c; yield from walk(c)
for w in walk(root):
    try: n=w.get_wm_name()
    except Exception: n=None
    if n=='Wallpaper UI': w.unmap()
for w in root.query_tree().children:
    g=w.get_geometry()
    if w.get_attributes().map_state==2 and min(g.width,g.height)<=20 and (g.width,g.height)!=(1,1): w.unmap()
d.sync(); time.sleep(2)
with mss.MSS(display=sys.argv[4]) as m:
    for i,p in enumerate(sys.argv[2].split()):
        f=p.split(','); x,y=int(f[0]),int(f[1]); act=f[2] if len(f)>2 else ''
        # small moves first so WE sees motion events, then the target
        for k in range(10):
            xtest.fake_input(d, 6, x=x+(10-k), y=y); d.sync(); time.sleep(0.05)
        xtest.fake_input(d, 6, x=x, y=y); d.sync()
        if act in ('d','c'):
            time.sleep(0.4); xtest.fake_input(d, 4, detail=1); d.sync()
        if act=='c': time.sleep(0.4)
        if act in ('u','c'):
            xtest.fake_input(d, 5, detail=1); d.sync()
        time.sleep(float(sys.argv[3]))
        s=m.grab({'left':0,'top':0,'width':1920,'height':1080})
        Image.frombytes('RGB',s.size,s.rgb).save('%s/f%d_%d_%d.png'%(sys.argv[1],i,x,y))
PY
/usr/lib/wine/wineserver -k 2>/dev/null
