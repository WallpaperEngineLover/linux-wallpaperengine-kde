#!/bin/bash
# sessenv.sh - run inside a VM over ssh: writes ~/sessenv with the variables of the logged in desktop session (taken from
# the environment of a client of the session), so commands run over ssh behave like they were started from the desktop.
# Usage: ./sessenv.sh; . ~/sessenv
vars="XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY DISPLAY XAUTHORITY XDG_SESSION_TYPE XDG_CURRENT_DESKTOP \
XDG_SESSION_DESKTOP XDG_SESSION_ID HYPRLAND_INSTANCE_SIGNATURE KDE_SESSION_VERSION KDE_FULL_SESSION DESKTOP_SESSION"
for name in plasmashell gsd-xsettings gnome-shell cosmic-panel Hyprland cinnamon xfce4-session xdg-desktop-portal; do
    pid=$(pgrep -u "$(id -u)" -x "$name" | head -1)
    [ -n "$pid" ] && break
done
[ -n "$pid" ] || { echo "no desktop session process found" >&2; exit 1; }
: > ~/sessenv
for v in $vars; do
    value=$(tr '\0' '\n' < "/proc/$pid/environ" | sed -n "s/^$v=//p" | head -1)
    [ -n "$value" ] && echo "export $v=\"$value\"" >> ~/sessenv
done
for v in $vars; do
    [ "$name" = Hyprland ] || break
    grep -q "^export $v=" ~/sessenv && continue
    value=$(systemctl --user show-environment 2>/dev/null | sed -n "s/^$v=//p" | head -1)
    [ -n "$value" ] && { echo "export $v=\"$value\"" >> ~/sessenv; continue; }
    for client in quickshell noctalia-shell kitty waybar; do
        cpid=$(pgrep -u "$(id -u)" -x "$client" | head -1)
        [ -n "$cpid" ] || continue
        value=$(tr '\0' '\n' < "/proc/$cpid/environ" | sed -n "s/^$v=//p" | head -1)
        [ -n "$value" ] && { echo "export $v=\"$value\"" >> ~/sessenv; break; }
    done
done
if [ "$name" = Hyprland ] && ! grep -q HYPRLAND_INSTANCE_SIGNATURE ~/sessenv; then
    # under uwsm the compositor's own environment lacks the signature, its socket directory has it
    sig=$(ls -t "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/hypr" 2>/dev/null | head -1)
    [ -n "$sig" ] && echo "export HYPRLAND_INSTANCE_SIGNATURE=\"$sig\"" >> ~/sessenv
fi
if [ "$name" = Hyprland ] && ! grep -q '^export XDG_SESSION_TYPE="wayland"' ~/sessenv; then
    # started from a tty login (no display manager) the session type is "tty"
    sed -i '/^export XDG_SESSION_TYPE=/d' ~/sessenv
    echo 'export XDG_SESSION_TYPE="wayland"' >> ~/sessenv
fi
if [ "$name" = gnome-shell ]; then
    # the shell's own environment lacks the display variables, its clients and the user manager have them
    for v in WAYLAND_DISPLAY DISPLAY XAUTHORITY; do
        grep -q "^export $v=" ~/sessenv && continue
        value=$(systemctl --user show-environment 2>/dev/null | sed -n "s/^$v=//p" | head -1)
        [ -n "$value" ] && echo "export $v=\"$value\"" >> ~/sessenv
    done
fi
echo "session of $name (pid $pid):"
cat ~/sessenv
