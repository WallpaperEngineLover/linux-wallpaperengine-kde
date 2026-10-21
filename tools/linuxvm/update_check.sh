#!/bin/bash
# update_check.sh <vm> <app dir | .deb> - we_manager's update flow in a VM's desktop session against fake_github.py on
# this host: an app release (the candidate's .deb with a newer Version) and an engine release (the portable tarball under
# a newer tag). Checks that both updates are offered, the engine update installs, and for a .deb install that the app
# update downloads, installs through polkit (a rule for the lab user stands in for the dialog) and the package version
# changes. Other install kinds (an unpacked AppImage, Flatpak) are expected to only get the notice. Needs the session
# running (sessenv) and the candidate's we_manager unpacked/installed the way wem_check.sh does.
#   APP_DEB       .deb to serve as the new app release (default: the one next to the app dir's dist folder)
#   ENGINE_TARBALL portable engine tarball to serve (default ~/.local/share/distro-rootfs/out/linux-wallpaperengine-kde-portable-x86_64.tar.gz)
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 app=$2
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
cdp() { sshv ". ~/sessenv; cd ~/wem; NODE_EXTRA_CA_CERTS= timeout ${CDP_TIMEOUT:-90} python3 cdp.py \"$1\" 2>&1 | cut -c1-${2:-700}"; }

work=$(mktemp -d)
engine_tag=v99.0.$(date +%s)   # newer than whatever engine the VM has, a golden disk may hold an earlier run's
deb=${APP_DEB:-}
if [ -z "$deb" ]; then
    for candidate in "$(dirname "$app")"/*.deb "$(dirname "$app")"/../*.deb; do [ -f "$candidate" ] && deb=$candidate && break; done
fi
engine=${ENGINE_TARBALL:-$HOME/.local/share/distro-rootfs/out/linux-wallpaperengine-kde-portable-x86_64.tar.gz}
app_release=()
if [ -f "$deb" ]; then
    python3 "$HERE/bump_deb.py" "$deb" "$work/we-manager_0.9.9_amd64.deb" 0.9.9
    app_release=(--release "WallpaperEngineLover/we_manager=v0.9.9:$work/we-manager_0.9.9_amd64.deb")
else
    echo "SKIP app update: no .deb to serve as the new app release (APP_DEB), only the engine update is checked"
fi
cp "$engine" "$work/linux-wallpaperengine-kde-portable-x86_64.tar.gz"
cp "$engine" "$work/linux-wallpaperengine-generic-portable-x86_64.tar.gz"
mkdir -p "$work/certs"
. "$HERE/fake_github_lib.sh"
fgh_start "$vm" "$work" \
    "${app_release[@]}" \
    --release "WallpaperEngineLover/linux-wallpaperengine-kde=$engine_tag:$work/linux-wallpaperengine-kde-portable-x86_64.tar.gz,$work/linux-wallpaperengine-generic-portable-x86_64.tar.gz"
trap 'kill $fake 2>/dev/null; rm -rf "$work"' EXIT
sleep 3
kill -0 $fake 2>/dev/null || { cat "$work/fake.log"; echo "fake_github did not start (port 443?)"; exit 2; }

sshv 'mkdir -p ~/wem'
if [ -d "$app" ] && ! sshv "test -x ~/wem/$(basename "$app")/we-manager"; then   # the app is not on the VM yet: copy it like wem_check.sh
    tar -C "$(dirname "$app")" -cf - "$(basename "$app")" | ssh -i "$HOME/.local/share/linuxvm/id_ed25519" -p "$(cat "$HOME/.local/share/linuxvm/vms/$vm/port")" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR lab@127.0.0.1 'mkdir -p ~/wem && tar -C ~/wem -xf -'
fi
"$HERE/linuxvm.sh" scp "$vm" "$work/certs/ca.pem" wem/fake-ca.pem
"$HERE/linuxvm.sh" scp "$vm" "$HERE/cdp.py" wem/cdp.py
sshv 'grep -q "api.github.com" /etc/hosts || echo "10.0.2.2 api.github.com" | sudo tee -a /etc/hosts >/dev/null
sudo mkdir -p /etc/polkit-1/rules.d
printf "polkit.addRule(function(action, subject) { if (subject.user == \"lab\") return polkit.Result.YES; });\n" | sudo tee /etc/polkit-1/rules.d/49-lab-test.rules >/dev/null
pkill -f "[w]e-manager" 2>/dev/null; for _ in $(seq 20); do pgrep -f "[w]e-manager" >/dev/null || break; sleep 1; done; true'
sleep 2
if [[ $app == *.deb ]]; then
    "$HERE/linuxvm.sh" scp "$vm" "$app" wem/app.deb
    sshv 'sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-downgrades ./wem/app.deb >/tmp/wem-deb.log 2>&1'
fi
if [[ $app == *.deb ]]; then start="${WEM_LAUNCH:-} we-manager ${WEM_FLAGS:-}"; else start="${WEM_LAUNCH:-} ~/wem/$(basename "$app")/we-manager --no-sandbox ${WEM_FLAGS:-}"; fi
sshv ". ~/sessenv; export NODE_EXTRA_CA_CERTS=\$HOME/wem/fake-ca.pem; (setsid nohup $start --remote-debugging-port=9222 > ~/wem/app-update.log 2>&1 </dev/null &); for _ in \$(seq 40); do grep -q 'DevTools listening' ~/wem/app-update.log 2>/dev/null && break; sleep 1; done; sleep 8"
[ "$(sshv 'pgrep -fc "[w]e-manager"')" -gt 0 ] && pass "we_manager starts" || { fail "we_manager did not start: $(sshv 'tail -3 ~/wem/app-update.log')"; exit 1; }

out=$(cdp 'window.electronAPI.updates.setChecks({app: true, engine: true})' 1500)
out=$(cdp 'window.electronAPI.updates.check()' 2500); echo "     $out" | tr '\n' ' ' | cut -c1-600; echo
kind=$(echo "$out" | grep -o '"appInstall": "[^"]*"' | cut -d'"' -f4)
if [ -f "$deb" ]; then
    echo "$out" | grep -q '"version": "0.9.9"\|"version": "v0.9.9"' && pass "app update 0.9.9 offered (install kind $kind)" || fail "app update not offered"
fi
if echo "$out" | grep -q "$engine_tag"; then pass "engine update $engine_tag offered"
elif echo "$out" | grep -q '"engineInstall": null'; then echo "SKIP engine update offer: no engine installed through we_manager on this VM yet"
else fail "engine update not offered"; fi

if [ "$kind" = deb ]; then
    out=$(cdp 'window.electronAPI.updates.installApp()' 1500); sleep 20
    version=$(sshv 'dpkg-query -W -f="\${Version}" we-manager 2>/dev/null')
    [ "$version" = 0.9.9 ] && pass "the .deb update installed (we-manager $version)" || fail "after installApp the package is $version: $out"
else
    echo "SKIP app install: install kind '$kind' only gets the notice"
fi

out=$(sshv ". ~/sessenv; cd ~/wem; timeout 400 python3 cdp.py 'window.electronAPI.lwe.installPrebuilt()' 2>&1 | cut -c1-300")
if echo "$out" | grep -q '"installed": true'; then pass "engine update installed through the prebuilt install: $(echo "$out" | grep -o '"version": "[^"]*"')"; else fail "engine update failed: $out"; fi
grep -c "GET" "$work/fake.log" | xargs -I{} echo "     {} requests reached the fake api.github.com"
sshv 'pkill -f "[w]e-manager" 2>/dev/null; for _ in $(seq 20); do pgrep -f "[w]e-manager" >/dev/null || break; sleep 1; done; sudo rm -f /etc/polkit-1/rules.d/49-lab-test.rules; sudo sed -i "/api.github.com/d" /etc/hosts; true'
exit $status
