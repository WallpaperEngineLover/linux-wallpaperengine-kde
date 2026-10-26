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
cdp() { sshv ". ~/sessenv; cd ~/wem; timeout 90 python3 cdp.py \"$1\" 2>&1 | cut -c1-600"; }
overlays() { sshv 'for p in $(pgrep -x python3); do tr "\0" " " < /proc/$p/cmdline; echo; done | sed -n "s/.*desktop-icons-overlay.py *\(.*\)/\1/p" | tr -d " " | sort | tr "\n" ";"'; }

sshv 'mkdir -p ~/wem; pkill -f "[w]e-manager"; pkill -x python3; pkill -x linux-wallpaper; true'
"$HERE/linuxvm.sh" scp "$vm" "$HERE/cdp.py" wem/cdp.py
tar -C "$(dirname "$app")" -cf - "$(basename "$app")" | ssh -i "$HOME/.local/share/linuxvm/id_ed25519" \
    -p "$(cat "$HOME/.local/share/linuxvm/vms/$vm/port")" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR lab@127.0.0.1 'rm -rf ~/wem/'"$(basename "$app")"'; mkdir -p ~/wem && tar -C ~/wem -xf -'
sshv 'mkdir -p ~/.steam/steam/steamapps/common/wallpaper_engine ~/.steam/steam/steamapps/workshop/content/431960
ln -sfn ~/lwe/assets ~/.steam/steam/steamapps/common/wallpaper_engine/assets
d=~/.steam/steam/steamapps/workshop/content/431960/1111948150; [ -d $d ] || cp -al ~/lwe/wp-1111948150 $d
mkdir -p ~/.local/share; [ -x ~/.local/share/linux-wallpaperengine-kde/linux-wallpaperengine ] || cp -a ~/lwe/lwe-hints ~/.local/share/linux-wallpaperengine-kde
d=$(xdg-user-dir DESKTOP); mkdir -p $d; touch $d/notes.txt $d/data.csv'
sshv ". ~/sessenv; (setsid nohup ~/wem/$(basename "$app")/we-manager --no-sandbox --remote-debugging-port=9222 > ~/wem/app.log 2>&1 </dev/null &); for _ in \$(seq 40); do grep -q 'DevTools listening' ~/wem/app.log 2>/dev/null && break; sleep 1; done; sleep 8"
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
