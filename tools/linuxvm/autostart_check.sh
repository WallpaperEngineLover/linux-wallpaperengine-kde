#!/bin/bash
# autostart_check.sh <vm> <app dir | installed> [screen] - the autostart of we_manager across a real reboot: makes a
# one wallpaper playlist through the running we_manager (DevTools, like wem_check.sh), sets it as the autostart playlist, reboots the VM and waits
# for the session. we_manager and the engine with its wallpaper must be running again afterwards. the engine has to be installed
# (wem_check.sh does it); LOGIN_PASSWORD types a password at a greeter without autologin; we_manager is started with --remote-debugging-port=9222 unless it already runs that way.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 screen=${3:-Virtual-1}
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
cdp() { sshv ". ~/sessenv; cd ~/wem; timeout ${CDP_TIMEOUT:-90} python3 cdp.py \"$1\" 2>&1 | cut -c1-500"; }

if ! cdp 'window.electronAPI.lwe.listScreens()' | grep -q "$screen"; then
    sshv 'pkill -f "[w]e-manager"; true'; sleep 3
    if [[ $2 == installed ]]; then start="${WEM_LAUNCH:-} we-manager ${WEM_FLAGS:-}"; else start="${WEM_LAUNCH:-} ~/wem/$(basename "$2")/we-manager --no-sandbox ${WEM_FLAGS:-}"; fi
    sshv ". ~/sessenv; (setsid nohup $start --remote-debugging-port=9222 > ~/wem/app-auto.log 2>&1 </dev/null &); sleep 25"
fi
sshv 'cd ~/.steam/steam/steamapps/workshop/content/431960 && for id in 1120596121 1078208425; do [ -L $id ] && rm $id; [ -d $id ] || cp -al ~/lwe/wp-$id $id; done'  # the scan skips symlinked folders
cdp 'window.electronAPI.library.scan()' >/dev/null
# an autostarted we_manager only brings wallpapers back through its autostart playlist (or a playlist that was playing)
out=$(cdp "(async () => { const p = await window.electronAPI.playlist.create('autostart-test'); await window.electronAPI.playlist.addItems(p.id, ['1120596121']); await window.electronAPI.config.setAutostart(true, true, p.id); return p.id })()")
if echo "$out" | grep -q 'Traceback\|rror'; then fail "playlist for the autostart: $out"; else pass "autostart playlist made: $out"; fi
entry=$(sshv 'grep "^Exec=" ~/.config/autostart/we-manager.desktop 2>/dev/null')
[ -n "$entry" ] && pass "autostart entry: $entry" || { fail "no autostart entry"; exit 1; }
[ -n "$WEM_FLAGS" ] && sshv "sed -i \"s|^\\(Exec=.*\\)\$|\\1 $WEM_FLAGS|\" ~/.config/autostart/we-manager.desktop"  # COSMIC in the VM needs --disable-gpu-compositing
sleep 5
sshv 'sync; sudo systemctl reboot' >/dev/null 2>&1
sleep 20
up=0
logged_in=0
for _ in $(seq 90); do
    # a greeter without autologin (COSMIC): type the password once the VM answers
    if [ -n "$LOGIN_PASSWORD" ] && [ $logged_in = 0 ] && sshv 'true' 2>/dev/null; then
        sleep 25; "$HERE/linuxvm.sh" type "$vm" "$LOGIN_PASSWORD"; "$HERE/linuxvm.sh" keys "$vm" ret; logged_in=1
    fi
    sshv 'true' 2>/dev/null && sshv 'bash ~/lwe/sessenv.sh' >/dev/null 2>&1 && { up=1; break; }
    sleep 4
done
[ $up = 1 ] && pass "the VM is back with its desktop session" || { fail "no desktop session after the reboot"; exit 1; }
for _ in $(seq 40); do
    [ "$(sshv 'pgrep -fc "[w]e-manager"')" -gt 0 ] && break
    sleep 3
done
[ "$(sshv 'pgrep -fc "[w]e-manager"')" -gt 0 ] && pass "we_manager started by the session" || fail "we_manager did not come back: $(sshv 'ls ~/.config/autostart')"
for _ in $(seq 40); do
    [ "$(sshv 'pgrep -fc "/[l]inux-wallpaperengine(\.bin)? "')" -gt 0 ] && break
    sleep 3
done
engines=$(sshv 'pgrep -fa "/[l]inux-wallpaperengine(\.bin)? " | head -1 | cut -c1-200')
[ -n "$engines" ] && pass "the engine came back: $engines" || fail "no engine after the reboot"
sleep 15
tmp=$(mktemp -d)
"$HERE/linuxvm.sh" shot "$vm" "$tmp/after.png" && echo "     screenshot of the desktop after the reboot: $tmp/after.png"
exit $status
