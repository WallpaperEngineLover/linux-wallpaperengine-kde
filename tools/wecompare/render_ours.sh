#!/bin/bash
# usage: render_ours.sh <outdir> [ids...]  (default: every item in the scan.py database)
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
ids=("$@"); [ ${#ids[@]} -eq 0 ] && ids=($(python3 -c "import json;print(' '.join(sorted(json.load(open('$LIB/features.json')))))"))
for id in "${ids[@]}"; do
  PULSE_SERVER=unix:/nonexistent-pulse PIPEWIRE_REMOTE=/nonexistent-pipewire \
  HEADLESS_RENDER_SIZE=1920x1058 HEADLESS_RENDER_DELAY=${DELAY:-600} HEADLESS_RENDER_TIMEOUT=90 LD_LIBRARY_PATH=$LWE \
    $SRC/tools/headless_render.sh $LWE/lwe-regress-bin "$OUT/$id.png" --assets-dir $A --fps 30 --silent $W/$id > "$OUT/$id.log" 2>&1 < /dev/null
  echo "ours $id rc=$? $( [ -f "$OUT/$id.png" ] && echo ok || echo NO-PNG)"
done
