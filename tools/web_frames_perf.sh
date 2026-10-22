#!/usr/bin/env bash
# CPU, memory and page frame rate of a web wallpaper with shm or dmabuf frames, on a private sway session (never the
# desktop's): the engine's main process renders headless on the GPU, the web host runs on the session's Wayland display.
#
#   tools/web_frames_perf.sh <shm|dmabuf> <workshop item dir> [WxH] [fps] [engine binary]
#
# Samples the CPU time of the engine and every Chromium process of it for 20 s after a 22 s warm-up, then prints the
# summed PSS and the host's own `page frames/s` lines (LWE_WEB_STATS). Run one at a time.
set -u
MODE=$1; ITEM=$2; SIZE=${3:-1920x1080}; FPS=${4:-60}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; REPO="$(dirname "$HERE")"
BIN=${5:-$REPO/build/output/linux-wallpaperengine}
ASSETS=${LWE_ASSETS_DIR:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
OUT=$(mktemp -d /tmp/lwe-webperf.XXXXXX)
ulimit -c 0
python3 "$HERE/desktop_session.py" start sway --size 1920x1080 > /dev/null 2>&1
eval "$(python3 "$HERE/desktop_session.py" env sway)"
export LWE_WEB_STATS=1 LWE_CEF_NO_SANDBOX=1 LWE_WEB_HOST_PRELOAD="$(dirname "$(readlink -f "$BIN")")/libcef.so" \
    LWE_CEF_SWITCHES="--ignore-gpu-blocklist --disable-dev-shm-usage"
env XDG_SESSION_TYPE=headless LD_LIBRARY_PATH="$(dirname "$BIN")" PULSE_SERVER=unix:/nonexistent PIPEWIRE_REMOTE=/nonexistent \
    timeout 80 "$BIN" --window "0x0x$SIZE" --screenshot "$OUT/shot.png" --screenshot-delay 100000 --assets-dir "$ASSETS" \
    --fps "$FPS" --web-frames "$MODE" "$ITEM" > "$OUT/log" 2>&1 &
sleep 22
MAIN=$(pgrep -f -- "--window 0x0x$SIZE" | head -1)
procs() { { echo "$MAIN"; pgrep -f "lwe-cef-"; pgrep -f -- "--web-host"; } | sort -u; }
cpu() { local t=0; for p in $(procs); do v=$(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null); t=$((t + ${v:-0})); done; echo $t; }
a=$(cpu); t0=$(date +%s.%N); sleep 20; b=$(cpu); t1=$(date +%s.%N)
echo "$MODE: total cpu $(echo "($b-$a)/100/($t1-$t0)*100" | bc -l | cut -c1-5)% of a core"
pss=0; for p in $(procs); do v=$(awk '/^Pss:/{print $2}' /proc/$p/smaps_rollup 2>/dev/null); pss=$((pss + ${v:-0})); done
echo "$MODE: PSS $((pss / 1024)) MB"
kill "$MAIN" 2>/dev/null; sleep 3
grep -E "first page|frames arrive|page frames" "$OUT/log" | sed 's/CWeb host [0-9]*: //' | tail -6
python3 "$HERE/desktop_session.py" stop sway > /dev/null 2>&1
rm -rf "$OUT"
