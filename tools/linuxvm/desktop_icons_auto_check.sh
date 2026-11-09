#!/bin/bash
# desktop_icons_auto_check.sh <vm> <we_manager dist/linux-unpacked | .deb> - we_manager's "only draw the overlay while a wallpaper is
# running" mode on a Plasma Wayland VM (the guest needs gtk4-layer-shell, ~/lwe from prep_guest.sh): the setting is on after a
# fresh start, an enabled overlay is not drawn on a bare desktop, starts with the engine and goes away when the engine is
# stopped in the app, killed with kill all, or killed from outside; with the mode off it stays up without an engine, and
# switching the mode on removes it again. The master switch off removes it while an engine runs. FRESH=0 keeps the app's profile.
# WEM_LAUNCH and WEM_FLAGS are added to the app's start line like in the other we_manager scripts.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 app=$2
FRESH=${FRESH:-1}
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
skip() { echo "SKIP $1"; }
. "$HERE/wem_lib.sh"
overlay_count() { sshv 'n=0; for p in $(pgrep -x python3); do tr "\0" " " < /proc/$p/cmdline | grep -q desktop-icons-overlay.py && n=$((n+1)); done; echo $n' | tr -d '[:space:]'; }
engines() { sshv 'pgrep -xc linux-wallpaper || true' | tr -d '\n'; }

# wait_overlay up|down <seconds>
wait_overlay() {
    local want=$1 end=$((SECONDS + $2)) n
    while [ $SECONDS -lt $end ]; do
        n=$(overlay_count)
        if [[ $n =~ ^[0-9]+$ ]]; then
            if [ "$want" = up ] && [ "$n" -ge 1 ]; then return 0; fi
            if [ "$want" = down ] && [ "$n" -eq 0 ]; then return 0; fi
        fi
        sleep 1
    done
    return 1
}
expect() { # expect <up|down> <seconds> <what>
    if wait_overlay "$1" "$2"; then pass "$3"; else fail "$3 (overlays: $(overlay_count))"; fi
}
launch() { cdp "window.electronAPI.lwe.launch('$WP_DIR/1111948150', {screen: '*'})" >/dev/null; }

for _ in $(seq 60); do sshv 'bash ~/lwe/sessenv.sh' >/dev/null 2>&1 && break; sleep 5; done
sshv 'bash ~/lwe/sessenv.sh' >/dev/null 2>&1 || { fail "no desktop session"; exit 1; }
session=$(sshv '. ~/sessenv; echo "$XDG_CURRENT_DESKTOP $XDG_SESSION_TYPE"')
if [[ $session != *KDE*wayland* ]]; then skip "the overlay only exists on Plasma Wayland ($session)"; exit 0; fi
layer_shell() { sshv 'bash -c "ls /usr/lib64/libgtk4-layer-shell.so* /usr/lib/libgtk4-layer-shell.so* /usr/lib/x86_64-linux-gnu/libgtk4-layer-shell.so* 2>/dev/null | head -1"'; }
if [ -z "$(layer_shell)" ]; then
    wem_layer_shell_install >/dev/null 2>&1
    if [ -z "$(layer_shell)" ]; then skip "gtk4-layer-shell can't be installed in the guest, the overlay can't run"; exit 0; fi
fi

sshv 'mkdir -p ~/wem; pkill -f "[w]e-manager"; pkill -x python3; pkill -x linux-wallpaper; true'
wem_wait_gone
[ "$FRESH" = 1 ] && sshv 'rm -rf ~/.config/we-manager'
wem_install || exit 1
wem_steam_lib 1111948150
sshv 'd=$(xdg-user-dir DESKTOP); mkdir -p $d; touch $d/notes.txt $d/data.csv'
wem_run || exit 1
WP_DIR=$(sshv 'echo $HOME')/.steam/steam/steamapps/workshop/content/431960

if [ "$FRESH" = 1 ]; then
    [ "$(cdp 'window.electronAPI.desktopIcons.getAuto()')" = true ] && pass "auto mode is on after a fresh start" || fail "auto mode is off after a fresh start"
fi
cdp 'window.electronAPI.desktopIcons.setAuto(true)' >/dev/null

cdp 'window.electronAPI.desktopIcons.setEnabled(true)' >/dev/null
sleep 6
[ "$(overlay_count)" -eq 0 ] && pass "no overlay on a bare desktop" || fail "overlay drawn without a wallpaper"

launch
expect up 40 "overlay starts with the wallpaper"
cdp 'window.electronAPI.lwe.stop()' >/dev/null
expect down 15 "overlay removed when the wallpaper is stopped"

launch
expect up 40 "overlay comes back with the next wallpaper"
cdp 'window.electronAPI.lwe.killAll()' >/dev/null
expect down 15 "overlay removed after kill all"

launch
expect up 40 "overlay up again"
sshv 'pkill -9 -x linux-wallpaper; true'
expect down 15 "overlay removed when the engine is killed from outside"

cdp 'window.electronAPI.desktopIcons.setAuto(false)' >/dev/null
expect up 15 "auto mode off: overlay up without a wallpaper"
cdp 'window.electronAPI.desktopIcons.setAuto(true)' >/dev/null
expect down 15 "auto mode switched on: overlay removed on a bare desktop"

launch
expect up 40 "overlay up with the wallpaper again"
cdp 'window.electronAPI.desktopIcons.setEnabled(false)' >/dev/null
expect down 15 "master switch off removes the overlay while the engine runs"
[ "$(engines)" -ge 1 ] && pass "engine still running" || fail "engine stopped by the master switch"
cdp 'window.electronAPI.desktopIcons.setEnabled(true)' >/dev/null
expect up 15 "master switch on: overlay up with the running engine"

cdp 'window.electronAPI.desktopIcons.setEnabled(false)' >/dev/null
cdp 'window.electronAPI.lwe.killAll()' >/dev/null
sshv 'pkill -f "[w]e-manager"; pkill -x python3; true'
exit $status
