#!/bin/bash
# desktop_icons_wem_check.sh <vm> <we_manager dist/linux-unpacked> - the real app's overlay start on a Plasma VM with several outputs
# (LINUXVM_OUTPUTS=3, kscreen_mirror.cpp makes Virtual-3 a copy of Virtual-2): one overlay per active screen, none for the
# mirrored output nor for one switched off under Settings -> Display, and the engine's screens are the same list. The guest
# needs gtk4-layer-shell, ~/lwe (prep_guest.sh) and the outputs enabled.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 app=$2
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
. "$HERE/wem_lib.sh"
overlays() { sshv 'for p in $(pgrep -x python3); do tr "\0" " " < /proc/$p/cmdline; echo; done | sed -n "s/.*desktop-icons-overlay.py *\(.*\)/\1/p" | tr -d " " | sort | tr "\n" ";"'; }

sshv 'mkdir -p ~/wem; pkill -f "[w]e-manager"; pkill -x python3; pkill -x linux-wallpaper; true'
wem_wait_gone
wem_install || exit 1
wem_steam_lib 1111948150
wem_engine_fallback
sshv 'd=$(xdg-user-dir DESKTOP); mkdir -p $d; touch $d/notes.txt $d/data.csv'
wem_run || exit 1
HOME_STEAM=$(sshv 'echo $HOME')/.steam/steam/steamapps/workshop/content/431960

screens=$(cdp 'window.electronAPI.lwe.listScreens()'); echo "     screens: $screens"
mirrored=$(cdp 'window.electronAPI.lwe.mirroredScreens ? window.electronAPI.lwe.mirroredScreens() : "n/a"'); echo "     mirrored: $mirrored"
cdp 'window.electronAPI.desktopIcons.setEnabled(true)' >/dev/null
cdp "window.electronAPI.lwe.launch('$HOME_STEAM/1111948150', {screen: '*'})" >/dev/null; sleep 20
got=$(overlays); echo "     overlays: $got"
for s in Virtual-1 Virtual-2; do case $got in *"$s;"*) pass "overlay for $s";; *) fail "no overlay for $s";; esac; done
case $got in *Virtual-3*) fail "overlay for the mirrored Virtual-3";; *) pass "no overlay for the mirrored Virtual-3";; esac

cdp "window.electronAPI.config.setDisabledScreens(['Virtual-2'])" >/dev/null
cdp 'window.electronAPI.lwe.stop()' >/dev/null; sleep 5
cdp "window.electronAPI.lwe.launch('$HOME_STEAM/1111948150', {screen: '*'})" >/dev/null; sleep 20
got=$(overlays); echo "     overlays with Virtual-2 switched off: $got"
case $got in *Virtual-2*|*Virtual-3*) fail "overlay for a switched off or mirrored screen";; *) pass "only Virtual-1 keeps an overlay";; esac
[ "$(sshv 'pgrep -xc python3')" -ge 1 ] && pass "overlay processes alive" || fail "overlay processes gone"
status_out=$(cdp 'window.electronAPI.desktopIcons.getStatus()'); echo "     status: $status_out"
cdp "window.electronAPI.config.setDisabledScreens([])" >/dev/null
cdp 'window.electronAPI.lwe.killAll()' >/dev/null
sshv 'pkill -f "[w]e-manager"; pkill -x python3; true'
exit $status
