#!/bin/bash
# usage: ours_window.sh <scene dir> <seconds> [engine args...]
# runs this engine as a 1920x1080 window on the isolated display :98, next to live WE (we_full.sh), for timing
# comparisons grabbed off the same X server. Same isolation as WE: no desktop Wayland/D-Bus, dead audio sockets.
# vblank_mode=0: Xwayland on :98 presents a vsynced GLX window at 1 Hz (weston's headless output never sends frame
# callbacks for it)
# LWE=<dir with linux-wallpaperengine> picks the build (default: this checkout's build/output)
ulimit -c 0
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
LWE=${LWE:-$(cd "$(dirname "$0")/../../.." && pwd)/build/output}
bash "$(dirname "$0")/we_display.sh" >/dev/null || exit 1
SCENE=$1 SECS=$2
shift 2
exec env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DBUS_SESSION_BUS_ADDRESS vblank_mode=0 DISPLAY=:98 \
    XDG_SESSION_TYPE=x11 XDG_RUNTIME_DIR=$S/run PULSE_SERVER=unix:$S/run/no-pulse \
    PIPEWIRE_REMOTE=$S/run/no-pipewire ALSA_CONFIG_PATH=/dev/null \
    timeout "$SECS" "$LWE/linux-wallpaperengine" --silent --window 0x0x1920x1080 \
    --assets-dir /workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets "$@" "$SCENE"
