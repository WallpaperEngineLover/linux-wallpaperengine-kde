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
. "$HERE/wem_lib.sh"

sshv 'mkdir -p ~/wem; pkill -f "[w]em/.*we-manager" 2>/dev/null; pkill -f "[w]e-manager" 2>/dev/null; true'
wem_wait_gone
wem_install || exit 1
wem_steam_lib 1120596121 1078208425
wem_run || exit 1
pass "we_manager starts"

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

engine_args() { sshv 'pgrep -af "/[l]inux-wallpaperengine(\.bin)? " | head -1'; }
apply_with_automute() {  # <wallpaper override: false|undefined>
    cdp "(async () => { const api = window.electronAPI; const id = '1120596121'; const cfg = await api.config.get(); \
      await api.config.setEngineFlags({ ...(cfg.engineFlags || {}), automute: true }); \
      await api.library.update(id, { engineFlags: $1 === false ? { automute: false } : undefined }); \
      return await api.wallpaper.apply({ wallpaperId: id }) })()" >/dev/null
    sleep 15
}
cdp 'window.electronAPI.library.scan()' >/dev/null
if cdp "window.electronAPI.library.getAll().then(a => a.some(w => w.id === '1120596121'))" | grep -q true; then
    apply_with_automute false
    args=$(engine_args)
    if [ -n "$args" ] && ! echo "$args" | grep -q -- '--automute'; then pass "automute ignore: a wallpaper on the list is launched without --automute"; else fail "automute ignore: engine args with the wallpaper on the list: ${args:-none running}"; fi
    apply_with_automute undefined
    args=$(engine_args)
    if echo "$args" | grep -q -- '--automute'; then pass "automute ignore: off the list the wallpaper is launched with --automute"; else fail "automute ignore: engine args after removing it from the list: ${args:-none running}"; fi
    cdp "(async () => { const api = window.electronAPI; const cfg = await api.config.get(); await api.config.setEngineFlags({ ...(cfg.engineFlags || {}), automute: false }) })()" >/dev/null
    cdp 'window.electronAPI.lwe.killAll()' >/dev/null; sleep 5
else
    echo "SKIP automute ignore: wallpaper 1120596121 is not in we_manager's library"
fi

cdp 'window.electronAPI.config.setAutostart(true, true, null)' >/dev/null
entry=$(sshv 'cat ~/.config/autostart/we-manager.desktop 2>/dev/null | grep "^Exec="')
[ -n "$entry" ] && pass "autostart entry: $entry" || fail "no autostart entry written"
exit $status
