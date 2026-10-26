#!/bin/bash
# usage: we_full.sh <scene dir or scene.json/scene.pkg> <out dir> [count] [interval seconds]
# runs on the GPU display :98 (DXVK); WE_DISPLAY=:97 WE_WINED3D=1 is the old Xvfb + wined3d (llvmpipe) path
# full window captures (1920x1058 at +4+30 on :97) of real WE 2.8.42 under Wine, see README.md
# WE_DESKTOP=1: desktop mode on monitor 0 via the wedesktop.c shell windows instead of -playInWindow
# WE_UNLOAD=1: open another wallpaper before stopping so WE writes bin/scenestorage (needs a real we28/bin, not a link)
# WE_WAIT=<seconds>: wait before the first grab (default 25, WE needs ~5 s to show the first frame)
# WE_LED=1: starts WE once on its own first (a patched wallpaper64.exe, see we_led.sh) so the LED plugin loads, then opens the scene
# WE_HRTF=1: sound objects render like on headphones: an alsoft.ini with stereo-mode = headphones next to
# wallpaper64.exe (OpenAL Soft 1.21.1 in mediaextensions64.dll reads it from the exe's folder), removed otherwise
ulimit -c 0
HERE=$(cd "$(dirname "$0")" && pwd)
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
D=${WE_DISPLAY:-:98}
[ "$D" = ":98" ] && { bash "$(dirname "$0")/we_display.sh" >/dev/null || exit 1; }
# WE_WINE_DIR=<other Wine build root> with its own WE_PREFIX (README); default is system Wine and $S/wineprefix
# WE_PROTON=<name> picks an installed Proton build (proton.sh) instead
[ -n "$WE_PROTON" ] && { eval "$(bash "$(dirname "$0")/proton.sh" env "$WE_PROTON")" || exit 1; }
WINE=${WE_WINE_DIR:+$WE_WINE_DIR/bin/wine}; [ -x /usr/lib/wine/wine64 ] && SYS_WINE=/usr/lib/wine/wine64 || SYS_WINE=$(command -v wine); WINE=${WINE:-$SYS_WINE}
WINESERVER=${WE_WINE_DIR:+$WE_WINE_DIR/bin/wineserver}; [ -x /usr/lib/wine/wineserver ] && SYS_WINESERVER=/usr/lib/wine/wineserver || SYS_WINESERVER=$(command -v wineserver); WINESERVER=${WINESERVER:-$SYS_WINESERVER}
export WINEPREFIX=${WE_PREFIX:-$S/wineprefix} DISPLAY=$D WINEDEBUG=${WE_WINEDEBUG:--all} DXVK_LOG_PATH=$S
# Wine must only see the isolated :98, not the desktop session (wayland-0, D-Bus) in the shared runtime dir
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS; export XDG_RUNTIME_DIR=$S/run
# keep Wine off the desktop's audio server (a shared /run/user/<uid>/pulse would play the wallpaper's sounds on real speakers and feed real system audio to the visualizers)
export PULSE_SERVER=unix:$S/run/no-pulse PIPEWIRE_REMOTE=$S/run/no-pipewire ALSA_CONFIG_PATH=/dev/null
# WE_AUDIO_REC=<file.wav>: record WE's audio from a private null sink (audio_sink.sh)
if [ -n "$WE_AUDIO_REC" ]; then
    bash "$(dirname "$0")/audio_sink.sh" start || exit 1
    eval "$(bash "$(dirname "$0")/audio_sink.sh" env)"
fi
[ -n "$WE_WINED3D" ] && export WINEDLLOVERRIDES="d3d11,d3d10core,dxgi=b"
[ "$D" != ":97" ] || pgrep -f "Xvfb :97" >/dev/null || { Xvfb :97 -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 & sleep 1; }
$WINESERVER -k 2>/dev/null; timeout 4 tail -f /dev/null
cd $S/we28
if [ -n "$WE_HRTF" ]; then printf '[general]\nstereo-mode = headphones\n' > alsoft.ini; else rm -f alsoft.ini; fi
F=$1; [ -d "$F" ] && F=$F/scene.json
WIN=$(echo "Z:$F" | tr '/' '\\')
if [ -n "$WE_AUDIO_REC" ]; then
    bash "$(dirname "$0")/audio_sink.sh" record "$WE_AUDIO_REC" 600 & REC=$!
    for i in $(seq 50); do pactl list short source-outputs 2>/dev/null | grep -q . && break; sleep 0.1; done
    sleep 0.5
fi
MODE=-playInWindow
if [ -n "$WE_DESKTOP" ]; then
    MODE=
    EXE=$S/wedesktop.exe
    if [ ! -f "$EXE" ] || [ "$HERE/wedesktop.c" -nt "$EXE" ]; then
        if command -v x86_64-w64-mingw32-gcc >/dev/null; then
            x86_64-w64-mingw32-gcc -O2 -municode -mwindows -o "$EXE" "$HERE/wedesktop.c" -luser32 -lgdi32 || exit 1
        else
            python3 -m ziglang cc -target x86_64-windows-gnu -O2 -municode -o "$EXE" "$HERE/wedesktop.c" -luser32 -lgdi32 || exit 1
        fi
    fi
    nohup $WINE "$EXE" > $S/wedesktop.log 2>&1 &
    timeout 4 tail -f /dev/null
fi
SILENT=-silent
if [ -n "$WE_LED" ]; then
    nohup $WINE wallpaper64.exe -silent > $S/we_pre.log 2>&1 &
    timeout 12 tail -f /dev/null
    SILENT=
fi
nohup $WINE wallpaper64.exe -control openWallpaper -file "$WIN" -monitor 0 $MODE $SILENT > $S/we_run_last.log 2>&1 &
timeout ${WE_WAIT:-25} tail -f /dev/null
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
        s=m.grab(m.monitors[0])
        Image.frombytes('RGB',s.size,s.rgb).save('%s/f%03d_%.1f.png'%(sys.argv[1],i,time.time()-t0))
        time.sleep(float(sys.argv[3]))
PY
if [ -n "$WE_UNLOAD" ]; then
    OTHER=$(echo "Z:${WE_UNLOAD_SCENE:-$HOME/.local/share/we_live/tests/keyorder/probe/scene.json}" | tr '/' '\\')
    $WINE wallpaper64.exe -control openWallpaper -file "$OTHER" -monitor 0 $MODE >> $S/we_run_last.log 2>&1
    timeout 10 tail -f /dev/null
fi
$WINESERVER -k 2>/dev/null
if [ -n "$REC" ]; then
    pkill -INT -P $REC timeout
    wait $REC || true
fi
