#!/bin/bash
# wem_check.sh <vm> <app dir | .deb> [screen] - we_manager in a VM's desktop session, driven over DevTools (cdp.py):
# distro detection and the prebuilt engine it picks, install from the GitHub release, launch of a scene and a web
# wallpaper, stop and kill all leaving nothing behind, autostart entry written. Needs the session running (sessenv).
# The app dir is we_manager's dist/linux-unpacked (what an AppImage unpacks to), a .deb is installed with the
# distribution's package manager first. Prints one PASS|FAIL line per check.
# WEM_FLAGS adds command line flags for the app (COSMIC needs --disable-gpu-compositing in the VM).
# SKIP_ENGINE_INSTALL=1 keeps the engine that is installed (update_check.sh leaves the candidate's there).
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 app=$2 screen=${3:-Virtual-1}
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
cdp() { sshv ". ~/sessenv; cd ~/wem; timeout ${CDP_TIMEOUT:-90} python3 cdp.py \"$1\" 2>&1 | cut -c1-600"; }

sshv 'mkdir -p ~/wem; pkill -f "[w]em/.*we-manager" 2>/dev/null; pkill -f "[w]e-manager" 2>/dev/null; for _ in $(seq 20); do pgrep -f "[w]e-manager" >/dev/null || break; sleep 1; done; true'
"$HERE/linuxvm.sh" scp "$vm" "$HERE/cdp.py" wem/cdp.py
if [[ $app == *.deb ]]; then
    "$HERE/linuxvm.sh" scp "$vm" "$app" wem/app.deb
    if sshv 'sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 install -y --allow-downgrades ./wem/app.deb >/tmp/wem-deb.log 2>&1'; then pass "the .deb installs"; else fail "the .deb did not install: $(sshv 'tail -3 /tmp/wem-deb.log')"; fi
    start="${WEM_LAUNCH:-} we-manager ${WEM_FLAGS:-}"
else
    tar -C "$(dirname "$app")" -cf - "$(basename "$app")" | ssh -i "$HOME/.local/share/linuxvm/id_ed25519" \
        -p "$(cat "$HOME/.local/share/linuxvm/vms/$vm/port")" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR lab@127.0.0.1 'mkdir -p ~/wem && tar -C ~/wem -xf -'
    start="${WEM_LAUNCH:-} ~/wem/$(basename "$app")/we-manager --no-sandbox ${WEM_FLAGS:-}"
fi

# a Steam library like a real user has: the Wallpaper Engine assets and the workshop items
sshv 'mkdir -p ~/.steam/steam/steamapps/common/wallpaper_engine ~/.steam/steam/steamapps/workshop/content/431960
ln -sfn ~/lwe/assets ~/.steam/steam/steamapps/common/wallpaper_engine/assets
for id in 1120596121 1078208425; do d=~/.steam/steam/steamapps/workshop/content/431960/$id; [ -L $d ] && rm $d; [ -d $d ] || cp -al ~/lwe/wp-$id $d; done'  # copies: the library scan skips symlinked folders

sshv ". ~/sessenv; (setsid nohup $start --remote-debugging-port=9222 > ~/wem/app.log 2>&1 </dev/null &); for _ in \$(seq 40); do grep -q 'DevTools listening' ~/wem/app.log 2>/dev/null && break; sleep 1; done; sleep 8"
[ "$(sshv 'pgrep -fc "[w]e-manager"')" -gt 0 ] && sshv "grep -q 'DevTools listening' ~/wem/app.log" && pass "we_manager starts" || { fail "we_manager did not start: $(sshv 'tail -3 ~/wem/app.log')"; exit 1; }

out=$(cdp 'window.electronAPI.lwe.detectDistro()'); echo "     distro: $out"
out=$(cdp 'window.electronAPI.lwe.prebuiltTarget()')
if echo "$out" | grep -q '"supported": true'; then pass "prebuilt engine offered: $(echo "$out" | grep -o '"asset": "[^"]*"')"; else fail "no prebuilt engine offered: $out"; fi
out=$(cdp 'window.electronAPI.lwe.listScreens()')
if echo "$out" | grep -q "$screen"; then pass "screens found: $out"; else fail "screen $screen not found: $out"; fi

if [ -z "$SKIP_ENGINE_INSTALL" ]; then
    sshv 'rm -rf ~/.local/share/linux-wallpaperengine-kde'
    out=$(sshv ". ~/sessenv; cd ~/wem; timeout 400 python3 cdp.py 'window.electronAPI.lwe.installPrebuilt()' 2>&1 | cut -c1-300")
    if echo "$out" | grep -q '"installed": true'; then pass "engine installed from the release: $(echo "$out" | grep -o '"version": "[^"]*"')"; else fail "engine install failed: $out"; fi
fi

if [ -n "$SKIP_ENGINE_INSTALL" ] && ! sshv 'test -x ~/.local/share/linux-wallpaperengine-kde/linux-wallpaperengine'; then
    # a read-only system skips the clean install and the update check has nothing to update: put the candidate where the install would
    sshv 'mkdir -p ~/.local/share && rm -rf ~/.local/share/linux-wallpaperengine-kde && cp -a ~/lwe/lwe-hints ~/.local/share/linux-wallpaperengine-kde'
    echo "     no engine was installed through we_manager here, copied the candidate into ~/.local/share/linux-wallpaperengine-kde"
fi

launch() { cdp "window.electronAPI.lwe.launch('$HOME_STEAM/$1', {screen: '$screen'})"; }
HOME_STEAM=$(sshv 'echo $HOME')/.steam/steam/steamapps/workshop/content/431960
for id in 1120596121 1078208425; do
    out=$(launch "$id"); sleep 15
    engines=$(sshv 'pgrep -fc "/[l]inux-wallpaperengine(\.bin)? "')
    if echo "$out" | grep -q '"running": true' && [ "$engines" -gt 0 ]; then pass "$id launched from we_manager ($engines engine processes)"; else fail "$id did not launch: $out"; fi
    cdp 'window.electronAPI.lwe.stop()' >/dev/null; sleep 5
    left=$(sshv 'echo $(pgrep -fc "/[l]inux-wallpaperengine(\.bin)? ") $(pgrep -fc "[w]eb-helper")')
    [ "$left" = "0 0" ] && pass "$id: stop leaves nothing behind" || fail "$id: after stop engines/helpers: $left"
done
launch 1078208425 >/dev/null; sleep 12
cdp 'window.electronAPI.lwe.killAll()' >/dev/null; sleep 6
left=$(sshv 'echo $(pgrep -fc "/[l]inux-wallpaperengine(\.bin)? ") $(pgrep -fc "[w]eb-helper")')
[ "$left" = "0 0" ] && pass "kill all leaves nothing behind" || fail "after kill all engines/helpers: $left"

cdp 'window.electronAPI.config.setAutostart(true, true, null)' >/dev/null
entry=$(sshv 'cat ~/.config/autostart/we-manager.desktop 2>/dev/null | grep "^Exec="')
[ -n "$entry" ] && pass "autostart entry: $entry" || fail "no autostart entry written"
exit $status
