#!/bin/bash
# run.sh <video> [seconds] - builds sharedvideo.exe with the MinGW toolchain in ~/.local/share/mingw-root (Fedora
# mingw64-gcc rpms unpacked, no root needed) and runs it under the we_live prefix on the isolated GPU display :98
ulimit -c 0
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
HERE=$(cd "$(dirname "$0")" && pwd)
# WE_PROTON=<name> picks an installed Proton build (proton.sh) instead
[ -n "$WE_PROTON" ] && { eval "$(bash "$HERE/../proton.sh" env "$WE_PROTON")" || exit 1; }
OUT=${SV_OUT:-$S/sharedvideo}
mkdir -p "$OUT"
CXX=${MINGW_CXX:-$HOME/.local/share/mingw-root/usr/bin/x86_64-w64-mingw32-g++}
$CXX -O1 -static -o "$OUT/sharedvideo.exe" "$HERE/sharedvideo.cpp" -ld3d11 -ldxgi -lmfplat -lmfuuid -lole32 \
    -loleaut32 -luuid -ld3dcompiler -lmfreadwrite || exit 1
bash "$HERE/../we_display.sh" >/dev/null || exit 1
cp "$1" "$OUT/video.mp4"
cd "$OUT"
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS
export XDG_RUNTIME_DIR=$S/run WINEPREFIX=${WE_PREFIX:-$S/wineprefix} DISPLAY=:98 WINEDEBUG=${WE_WINEDEBUG:--all}
export DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-warn} DXVK_LOG_PATH=$OUT
export PULSE_SERVER=unix:$S/run/no-pulse PIPEWIRE_REMOTE=$S/run/no-pipewire ALSA_CONFIG_PATH=/dev/null
WINE=${WE_WINE_DIR:+$WE_WINE_DIR/bin/wine}
timeout 60 ${WINE:-wine} sharedvideo.exe video.mp4 "${2:-2}" 2>&1 | grep -v "^info: \|fixme:\|not a conformant\|GStreamer-Video-CRITICAL\|^$"
