#!/bin/bash
# usage: render_ours.sh <outdir> [ids...]  (default: every scene/preset item in the scan.py database)
# headless GPU render at WE's window size (1920x1058), frame 600 @30fps -> <outdir>/<id>.png + .log
# LWE=<dir with linux-wallpaperengine + its lib .so> picks the build (default: this checkout's build/output)
# audio goes to dead sockets: the engine captures from the desktop's audio server even with --silent
W=${LWE_WORKSHOP_DIR:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
A=${LWE_ASSETS_DIR:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
LIB=${LWE_LIBRARY_DIR:-$HOME/.local/share/lwe-library}
SRC=$(cd "$(dirname "$0")/../.." && pwd)
LWE=${LWE:-$SRC/build/output}
OUT=$1; shift; mkdir -p "$OUT"
# a copy under another name so we_manager's shutdown pkill doesn't hit it
cp -f $LWE/linux-wallpaperengine $LWE/lwe-regress-bin
# web and video wallpapers can't be compared (see README) and only add CEF/mpv crashes, ALL_TYPES=1 keeps them
ids=("$@"); [ ${#ids[@]} -eq 0 ] && ids=($(python3 -c "
import json, os
items = json.load(open('$LIB/features.json'))
keep = os.environ.get('ALL_TYPES') == '1'
print(' '.join(sorted(i for i, v in items.items() if keep or v['info'].get('type') not in ('web', 'video'))))"))
# renders are paced at 30 fps (600 frames = 20 s of wall clock each), so they run JOBS at a time (default 6).
# headless_render.sh gives every render its own runtime dir; its D-Bus shim is built once up front
[ -f /tmp/dbus_noop_shim.so ] || gcc -shared -fPIC -o /tmp/dbus_noop_shim.so $SRC/tools/dbus_noop_shim.c -ldl
# WE's pointer on :98 (960,540 = window 956,510), fractions from the bottom
export LWE_HEADLESS_CURSOR=${LWE_HEADLESS_CURSOR:-0.4990,0.5143}
render_one() {
  local id=$1
  PULSE_SERVER=unix:/nonexistent-pulse PIPEWIRE_REMOTE=/nonexistent-pipewire \
  HEADLESS_RENDER_SIZE=1920x1058 HEADLESS_RENDER_DELAY=${DELAY:-600} HEADLESS_RENDER_TIMEOUT=90 LD_LIBRARY_PATH=$LWE${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
    $SRC/tools/headless_render.sh $LWE/lwe-regress-bin "$OUT/$id.png" --assets-dir $A --fps 30 --silent $W/$id > "$OUT/$id.log" 2>&1 < /dev/null
  echo "ours $id rc=$? $( [ -f "$OUT/$id.png" ] && echo ok || echo NO-PNG)"
}
export -f render_one
export SRC LWE OUT A W DELAY
printf '%s\n' "${ids[@]}" | xargs -P "${JOBS:-6}" -I{} bash -c 'render_one {}'
