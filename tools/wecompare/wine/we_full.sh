#!/bin/bash
# usage: we_full.sh <scene dir or scene.json/scene.pkg> <out dir> [count] [interval seconds]
# runs on the GPU display :98 (DXVK); WE_DISPLAY=:97 WE_WINED3D=1 is the old Xvfb + wined3d (llvmpipe) path
# full window captures (1920x1058 at +4+30 on :97) of real WE 2.8.42 under Wine, see README.md
ulimit -c 0
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
D=${WE_DISPLAY:-:98}
[ "$D" = ":98" ] && { bash "$(dirname "$0")/we_display.sh" >/dev/null || exit 1; }
export WINEPREFIX=$S/wineprefix DISPLAY=$D WINEDEBUG=-all DXVK_LOG_PATH=$S
# Wine must only see the isolated :98, not the desktop session (wayland-0, D-Bus) in the shared runtime dir
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS; export XDG_RUNTIME_DIR=$S/run
# keep Wine off the desktop's audio server (a shared /run/user/<uid>/pulse would play the wallpaper's sounds on real speakers and feed real system audio to the visualizers)
export PULSE_SERVER=unix:$S/run/no-pulse PIPEWIRE_REMOTE=$S/run/no-pipewire ALSA_CONFIG_PATH=/dev/null
[ -n "$WE_WINED3D" ] && export WINEDLLOVERRIDES="d3d11,d3d10core,dxgi=b"
[ "$D" != ":97" ] || pgrep -f "Xvfb :97" >/dev/null || { Xvfb :97 -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 & sleep 1; }
/usr/lib/wine/wineserver -k 2>/dev/null; timeout 4 tail -f /dev/null
cd $S/we28
F=$1; [ -d "$F" ] && F=$F/scene.json
WIN=$(echo "Z:$F" | tr '/' '\\')
nohup /usr/lib/wine/wine64 wallpaper64.exe -control openWallpaper -file "$WIN" -monitor 0 -playInWindow -silent > $S/we_run_last.log 2>&1 &
timeout 25 tail -f /dev/null
mkdir -p "$2"
python3 - "$2" "${3:-10}" "${4:-1}" "$D" <<'PY'
import sys, time, mss
from PIL import Image
from Xlib import display
d=display.Display(sys.argv[4]); root=d.screen().root
def walk(w):
    for c in w.query_tree().children:
        yield c; yield from walk(c)
for w in walk(root):
    try: n=w.get_wm_name()
    except Exception: n=None
    if n=='Wallpaper UI': w.unmap()
# the hidden UI leaves its border/shadow strips (8px override-redirect windows) and a small bar on top
for w in root.query_tree().children:
    g=w.get_geometry()
    if w.get_attributes().map_state==2 and min(g.width,g.height)<=20 and (g.width,g.height)!=(1,1): w.unmap()
d.sync(); time.sleep(2)
t0=time.time()
with mss.MSS(display=sys.argv[4]) as m:
    for i in range(int(sys.argv[2])):
        s=m.grab({'left':0,'top':0,'width':1920,'height':1080})
        Image.frombytes('RGB',s.size,s.rgb).save('%s/f%03d_%.1f.png'%(sys.argv[1],i,time.time()-t0))
        time.sleep(float(sys.argv[3]))
PY
/usr/lib/wine/wineserver -k 2>/dev/null
