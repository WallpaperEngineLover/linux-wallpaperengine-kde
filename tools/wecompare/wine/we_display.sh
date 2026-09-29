#!/bin/bash
# starts the isolated GPU display :98 (headless weston + Xwayland, private XDG_RUNTIME_DIR) if it isn't running
# :0 is the real desktop, weston's own xwayland module would try it first, so Xwayland is started here instead
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
R=$S/run
mkdir -p $R && chmod 700 $R
[ -f $S/weston.ini ] || printf '[core]\nxwayland=false\n[output]\nname=headless\nmode=1920x1080\n' > $S/weston.ini
if ! pgrep -x weston >/dev/null; then
    rm -f $R/we-wl*
    env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR=$R setsid nohup weston --backend=headless --renderer=gl \
        --width=1920 --height=1080 --socket=we-wl --config=$S/weston.ini --log=$R/weston.log >/dev/null 2>&1 &
    sleep 3
fi
if ! pgrep -f "Xwayland :98" >/dev/null; then
    env -u DISPLAY XDG_RUNTIME_DIR=$R WAYLAND_DISPLAY=we-wl setsid nohup Xwayland :98 -geometry 1920x1080 -nolisten tcp \
        > $R/xwayland.log 2>&1 &
    sleep 3
fi
DISPLAY=:98 xdpyinfo >/dev/null 2>&1 && echo ":98 ready" || { echo ":98 failed, see $R"; exit 1; }
