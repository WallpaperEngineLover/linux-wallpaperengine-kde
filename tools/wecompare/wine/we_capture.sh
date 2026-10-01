#!/bin/bash
# usage: we_capture.sh <scene dir or scene.json/scene.pkg> [seconds before capture]
# RenderDoc capture of one frame of real WE 2.8.42 (DXVK -> Vulkan on the GPU, isolated display :98).
# Prints the .rdc path; inspect it with tools/rdc_dump.py using DISPLAY=:98 (Xvfb has no DRI3, replay output asserts there)
ulimit -c 0
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
bash "$(dirname "$0")/we_display.sh" >/dev/null || exit 1
pgrep -f "Xvfb :97" >/dev/null || { Xvfb :97 -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 & sleep 1; }
export WINEPREFIX=$S/wineprefix DISPLAY=:98 WINEDEBUG=-all DXVK_LOG_PATH=$S
# Wine must only see the isolated :98, not the desktop session (wayland-0, D-Bus) in the shared runtime dir
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS; export XDG_RUNTIME_DIR=$S/run
# keep Wine off the desktop's audio server (a shared /run/user/<uid>/pulse would play the wallpaper's sounds on real speakers and feed real system audio to the visualizers)
export PULSE_SERVER=unix:$S/run/no-pulse PIPEWIRE_REMOTE=$S/run/no-pipewire ALSA_CONFIG_PATH=/dev/null
export VK_ADD_IMPLICIT_LAYER_PATH=$S/vklayer ENABLE_VULKAN_RENDERDOC_CAPTURE=1
/usr/lib/wine/wineserver -k 2>/dev/null; sleep 3
cd $S/we28
F=$1; [ -d "$F" ] && F=$F/scene.json
WIN=$(echo "Z:$F" | tr '/' '\\')
nohup /usr/lib/wine/wine64 wallpaper64.exe -control openWallpaper -file "$WIN" -monitor 0 -playInWindow -silent > $S/we_run_last.log 2>&1 &
sleep ${2:-30}
PID=$(pgrep -f "wallpaper64.exe -control" | head -1)
# qrenderdoc only has the xcb platform; it runs on the isolated Xvfb :97 (analytics already opted out in ~/.renderdoc)
env -u WAYLAND_DISPLAY -u VK_ADD_IMPLICIT_LAYER_PATH -u ENABLE_VULKAN_RENDERDOC_CAPTURE RD_PID=$PID RD_TRIGGER_OUT=$S/rd_trigger.out \
    QT_QPA_PLATFORM=xcb DISPLAY=:97 timeout 90 qrenderdoc --python $S/rd_trigger.py >/dev/null 2>&1
/usr/lib/wine/wineserver -k 2>/dev/null
head -1 $S/rd_trigger.out
