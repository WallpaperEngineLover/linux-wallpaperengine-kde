#!/bin/bash
# sessenv.sh - run inside a VM over ssh: writes ~/sessenv with the variables of the logged in desktop session (taken from
# the environment of a client of the session), so commands run over ssh behave like they were started from the desktop.
# Usage: ./sessenv.sh; . ~/sessenv
vars="XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY DISPLAY XAUTHORITY XDG_SESSION_TYPE XDG_CURRENT_DESKTOP \
XDG_SESSION_DESKTOP XDG_SESSION_ID HYPRLAND_INSTANCE_SIGNATURE KDE_SESSION_VERSION KDE_FULL_SESSION DESKTOP_SESSION"
for name in plasmashell gsd-xsettings cosmic-panel Hyprland cinnamon xfce4-session xdg-desktop-portal; do
    pid=$(pgrep -u "$(id -u)" -x "$name" | head -1)
    [ -n "$pid" ] && break
done
[ -n "$pid" ] || { echo "no desktop session process found" >&2; exit 1; }
: > ~/sessenv
for v in $vars; do
    value=$(tr '\0' '\n' < "/proc/$pid/environ" | sed -n "s/^$v=//p" | head -1)
    [ -n "$value" ] && echo "export $v=\"$value\"" >> ~/sessenv
done
echo "session of $name (pid $pid):"
cat ~/sessenv
