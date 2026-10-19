#!/bin/bash
# flatpak_check.sh <vm> <bundle.flatpak | installed> [screen] - the Flatpak build of we_manager in a VM's desktop session:
# installs the bundle (unless "installed"), starts it with DevTools (cdp.py reaches it, the sandbox shares the network),
# scans the library, launches a scene and a web wallpaper through it, checks stop and kill all leave nothing behind and
# the autostart entry starts the Flatpak. Needs flatpak and the 25.08 runtime + Electron BaseApp on the VM, and the
# session running (sessenv). One PASS|FAIL line per check.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 bundle=$2 screen=${3:-Virtual-1}
app=io.github.WallpaperEngineLover.we_manager
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
cdp() { sshv ". ~/sessenv; cd ~/wem; timeout ${CDP_TIMEOUT:-90} python3 cdp.py \"$1\" 2>&1 | cut -c1-500"; }
# gone: waits up to 20 s for the engines and web helpers to end, a stop in a Flatpak goes through flatpak-spawn
gone() { for _ in $(seq 20); do [ "$(left)" = "0 0" ] && return 0; sleep 1; done; return 1; }
left() { sshv 'echo $(pgrep -fc "/[l]inux-wallpaperengine(\.bin)? ") $(pgrep -fc "[w]eb-helper")'; }

sshv 'mkdir -p ~/wem'
"$HERE/linuxvm.sh" scp "$vm" "$HERE/cdp.py" wem/cdp.py
if [ "$bundle" != installed ]; then
    "$HERE/linuxvm.sh" scp "$vm" "$bundle" wem/app.flatpak
    sshv "flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo; flatpak --user install -y --noninteractive --reinstall ~/wem/app.flatpak >/tmp/fp-install.log 2>&1" && pass "the bundle installs" || fail "bundle install: $(sshv 'tail -3 /tmp/fp-install.log')"
fi
sshv 'pkill -f "[w]e-manager" 2>/dev/null; true'
sshv 'mkdir -p ~/.steam/steam/steamapps/common/wallpaper_engine ~/.steam/steam/steamapps/workshop/content/431960
ln -sfn ~/lwe/assets ~/.steam/steam/steamapps/common/wallpaper_engine/assets
cd ~/.steam/steam/steamapps/workshop/content/431960; for id in 1120596121 1078208425; do [ -L $id ] && rm $id; [ -d $id ] || cp -al ~/lwe/wp-$id $id; done'
sshv ". ~/sessenv; (setsid nohup flatpak run $app --remote-debugging-port=9222 > ~/wem/fp-app.log 2>&1 </dev/null &); sleep 30"
[ "$(sshv 'pgrep -fc "[w]e-manager"')" -gt 0 ] && pass "the Flatpak starts" || { fail "the Flatpak did not start: $(sshv 'tail -3 ~/wem/fp-app.log')"; exit 1; }

out=$(cdp 'window.electronAPI.lwe.prebuiltTarget()'); echo "     prebuilt target: $out" | cut -c1-200
out=$(cdp 'window.electronAPI.lwe.status ? window.electronAPI.lwe.status() : window.electronAPI.lwe.getStatus()')
echo "$out" | grep -q '"installed": true' && pass "the bundled engine is found: $(echo "$out" | grep -o '"path": "[^"]*"')" || fail "no engine inside the Flatpak: $out"
out=$(cdp 'window.electronAPI.lwe.listScreens()')
echo "$out" | grep -q "$screen" && pass "screens found through the host: $out" || fail "screen $screen not found: $out"
cdp 'window.electronAPI.library.scan()' > /dev/null
home=$(sshv 'echo $HOME')/.steam/steam/steamapps/workshop/content/431960
for id in 1120596121 1078208425; do
    out=$(cdp "window.electronAPI.lwe.launch('$home/$id', {screen: '$screen'})"); sleep 35
    engines=$(sshv 'pgrep -fc "/[l]inux-wallpaperengine(\.bin)? "')
    if echo "$out" | grep -q '"running": true' && [ "$engines" -gt 0 ]; then pass "$id launched from the Flatpak ($engines engine processes)"; else fail "$id did not launch: $out"; fi
    cdp 'window.electronAPI.lwe.stop()' >/dev/null
    gone && pass "$id: stop leaves nothing behind" || fail "$id: after stop engines/helpers: $(left)"
done
cdp "window.electronAPI.lwe.launch('$home/1078208425', {screen: '$screen'})" >/dev/null; sleep 30
cdp 'window.electronAPI.lwe.killAll()' >/dev/null
gone && pass "kill all leaves nothing behind" || fail "after kill all engines/helpers: $(left)"

cdp 'window.electronAPI.config.setAutostart(true, true, null)' >/dev/null
entry=$(sshv 'grep "^Exec=" ~/.config/autostart/*.desktop 2>/dev/null')
echo "$entry" | grep -q "flatpak run" && pass "autostart starts the Flatpak: $entry" || fail "autostart entry does not start the Flatpak: $entry"
sshv 'pkill -f "[w]e-manager" 2>/dev/null; true'
exit $status
