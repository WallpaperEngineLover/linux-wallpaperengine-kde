#!/bin/bash
# update_check.sh <vm> <app dir | .deb> - we_manager's update flow in a VM's desktop session against fake_github.py on
# this host: an app release (the candidate's .deb with a newer Version) and an engine release (the portable tarball under
# a newer tag). Checks that both updates are offered, the engine update installs, and for a .deb install that the app
# update downloads, installs through polkit (a rule for the lab user stands in for the dialog) and the package version
# changes. Also: a manual "check now" with the automatic checks off, "close" (until the next start) against "don't notify
# again" (kept), the version picker's list (only v-tagged releases, a libnode-arch helper release is left out) and going back
# to an older engine (prebuilt) and app (.deb) release. Other install kinds (an unpacked AppImage, Flatpak) are expected to only get the notice. Needs the session
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
engine_old=v98.0.${engine_tag##*.}
deb=${APP_DEB:-}
if [ -z "$deb" ]; then
    for candidate in "$(dirname "$app")"/*.deb "$(dirname "$app")"/../*.deb; do [ -f "$candidate" ] && deb=$candidate && break; done
fi
engine=${ENGINE_TARBALL:-$HOME/.local/share/distro-rootfs/out/linux-wallpaperengine-kde-portable-x86_64.tar.gz}
app_release=()
if [ -f "$deb" ]; then
    python3 "$HERE/bump_deb.py" "$deb" "$work/we-manager_0.9.9_amd64.deb" 0.9.9
    python3 "$HERE/bump_deb.py" "$deb" "$work/we-manager_0.9.7_amd64.deb" 0.9.7
    app_release=(--release "WallpaperEngineLover/we_manager=v0.9.9:$work/we-manager_0.9.9_amd64.deb"
                 --release "WallpaperEngineLover/we_manager=v0.9.7:$work/we-manager_0.9.7_amd64.deb")
else
    echo "SKIP app update: no .deb to serve as the new app release (APP_DEB), only the engine update is checked"
fi
cp "$engine" "$work/linux-wallpaperengine-kde-portable-x86_64.tar.gz"
cp "$engine" "$work/linux-wallpaperengine-generic-portable-x86_64.tar.gz"
echo helper > "$work/libnode-arch.pkg.tar.zst"
mkdir -p "$work/certs"
. "$HERE/fake_github_lib.sh"
fgh_start "$vm" "$work" \
    "${app_release[@]}" \
    --release "WallpaperEngineLover/linux-wallpaperengine-kde=$engine_tag:$work/linux-wallpaperengine-kde-portable-x86_64.tar.gz,$work/linux-wallpaperengine-generic-portable-x86_64.tar.gz" \
    --release "WallpaperEngineLover/linux-wallpaperengine-kde=!libnode-arch:$work/libnode-arch.pkg.tar.zst" \
    --release "WallpaperEngineLover/linux-wallpaperengine-kde=$engine_old:$work/linux-wallpaperengine-kde-portable-x86_64.tar.gz,$work/linux-wallpaperengine-generic-portable-x86_64.tar.gz"
trap 'kill $fake 2>/dev/null; rm -rf "$work"' EXIT
sleep 3
kill -0 $fake 2>/dev/null || { cat "$work/fake.log"; echo "fake_github did not start (port 443?)"; exit 2; }

sshv 'mkdir -p ~/wem'
if [ -d "$app" ]; then   # always the candidate: a golden disk keeps an older build, and clean_install_check.sh skips read-only systems
    sshv 'pkill -f "[w]e-manager" 2>/dev/null; for _ in $(seq 20); do pgrep -f "[w]e-manager" >/dev/null || break; sleep 1; done; rm -rf ~/wem/'"$(basename "$app")"'; true'
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
    sshv 'sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 install -y --allow-downgrades ./wem/app.deb >/tmp/wem-deb.log 2>&1'
fi
if [[ $app == *.deb ]]; then start="${WEM_LAUNCH:-} we-manager ${WEM_FLAGS:-}"; else start="${WEM_LAUNCH:-} ~/wem/$(basename "$app")/we-manager --no-sandbox ${WEM_FLAGS:-}"; fi
sshv ". ~/sessenv; export NODE_EXTRA_CA_CERTS=\$HOME/wem/fake-ca.pem; (setsid nohup $start --remote-debugging-port=9222 > ~/wem/app-update.log 2>&1 </dev/null &); for _ in \$(seq 40); do grep -q 'DevTools listening' ~/wem/app-update.log 2>/dev/null && break; sleep 1; done; sleep 8"
[ "$(sshv 'pgrep -fc "[w]e-manager"')" -gt 0 ] && pass "we_manager starts" || { fail "we_manager did not start: $(sshv 'tail -3 ~/wem/app-update.log')"; exit 1; }

val() { cdp "$1" 400 | tr -d '"'; }
cdp 'window.electronAPI.updates.setChecks({app: false, engine: false})' 300 >/dev/null
sleep 3
manual=$(val "window.electronAPI.updates.check().then(s => [s.app && s.app.version, s.checked.app, s.checked.engine].join(' '))")
want="v0.9.9 true true"
[ -f "$deb" ] || want=" true true"
[ "$manual" = "$want" ] && pass "manual check with automatic checks off looked at everything ($manual)" || fail "manual check with automatic checks off: '$manual', wanted '$want'"

out=$(cdp 'window.electronAPI.updates.setChecks({app: true, engine: true})' 1500)
out=$(cdp 'window.electronAPI.updates.check()' 2500); echo "     $out" | tr '\n' ' ' | cut -c1-600; echo
kind=$(echo "$out" | grep -o '"appInstall": "[^"]*"' | cut -d'"' -f4)
if [ -f "$deb" ]; then
    echo "$out" | grep -q '"version": "0.9.9"\|"version": "v0.9.9"' && pass "app update 0.9.9 offered (install kind $kind)" || fail "app update not offered"
fi
if echo "$out" | grep -q "$engine_tag"; then pass "engine update $engine_tag offered"
elif echo "$out" | grep -q '"engineInstall": null'; then echo "SKIP engine update offer: no engine installed through we_manager on this VM yet"
else fail "engine update not offered"; fi

r=$(val "window.electronAPI.updates.dismiss('app', 'v0.9.9', false).then(s => [s.snoozed.app, s.dismissed.app].join('|'))")
[ "$r" = "v0.9.9|" ] && pass "closing a notice only snoozes it" || fail "close: '$r'"
r=$(val "window.electronAPI.updates.dismiss('app', 'v0.9.9', true).then(s => s.dismissed.app)")
[ "$r" = v0.9.9 ] && pass "don't notify me again keeps the version dismissed" || fail "dismiss for good: '$r'"
r=$(val "window.electronAPI.updates.check().then(s => Object.keys(s.dismissed).length + '/' + Object.keys(s.snoozed).length)")
[ "$r" = 0/0 ] && pass "check now brings dismissed notices back" || fail "check now after dismissing: '$r'"

r=$(val "window.electronAPI.updates.versions('engine').then(v => v.map(x => x.tag).join(' '))")
if [[ $r == *"$engine_tag"* && $r == *"$engine_old"* && $r != *libnode* ]]; then pass "engine versions listed without the libnode release ($r)"; else fail "engine versions: '$r'"; fi
if [ -f "$deb" ]; then
    r=$(val "window.electronAPI.updates.versions('app').then(v => v.map(x => x.tag).join(' '))")
    want="v0.9.9 v0.9.7"
    [ "$kind" = deb ] || want=""
    [ "$r" = "$want" ] && pass "app versions for a '$kind' install: '$r'" || fail "app versions: '$r', wanted '$want'"
fi

if [ "$kind" = deb ]; then
    out=$(cdp 'window.electronAPI.updates.installApp()' 1500); sleep 20
    version=$(sshv 'dpkg-query -W -f="\${Version}" we-manager 2>/dev/null')
    [ "$version" = 0.9.9 ] && pass "the .deb update installed (we-manager $version)" || fail "after installApp the package is $version: $out"
else
    echo "SKIP app install: install kind '$kind' only gets the notice"
fi

# the app restarts after its own .deb update, a loaded guest takes a while before the debugging port answers again
for _ in $(seq 30); do [ "$(cdp '1 + 1' 20)" = 2 ] && break; sleep 3; done
# an app that restarts after its own update can drop the first call (empty answer), ask again
for _ in 1 2 3; do
    out=$(sshv ". ~/sessenv; cd ~/wem; timeout 400 python3 cdp.py 'window.electronAPI.lwe.installPrebuilt()' 2>&1 | cut -c1-300")
    [ -n "$out" ] && break
    sleep 20
done
if echo "$out" | grep -q '"installed": true'; then pass "engine update installed through the prebuilt install: $(echo "$out" | grep -o '"version": "[^"]*"')"; else fail "engine update failed: $out"; fi

# the .deb needs apt's --allow-downgrades to go back
if echo "$out" | grep -q '"installed": true'; then
    out=$(sshv ". ~/sessenv; cd ~/wem; timeout 400 python3 cdp.py \"window.electronAPI.lwe.installPrebuilt('$engine_old')\" 2>&1 | cut -c1-300")
    echo "$out" | grep -q "\"version\": \"$engine_old\"" && pass "engine went back to $engine_old" || fail "engine downgrade to $engine_old: $out"
    out=$(sshv ". ~/sessenv; cd ~/wem; timeout 400 python3 cdp.py \"window.electronAPI.lwe.installPrebuilt('libnode-arch')\" 2>&1 | cut -c1-300")
    echo "$out" | grep -q "\"version\": \"$engine_old\"" && pass "a non-version release is refused, the engine stays on $engine_old" || fail "libnode-arch changed the engine: $out"
fi
if [ "$kind" = deb ]; then
    for _ in $(seq 30); do [ "$(cdp '1 + 1' 20)" = 2 ] && break; sleep 3; done
    out=$(cdp "window.electronAPI.updates.installApp('v0.9.7')" 1500); sleep 20
    version=$(sshv 'dpkg-query -W -f="\${Version}" we-manager 2>/dev/null')
    [ "$version" = 0.9.7 ] && pass "the .deb went back to 0.9.7" || fail "after installApp('v0.9.7') the package is $version: $out"
fi
grep -c "GET" "$work/fake.log" | xargs -I{} echo "     {} requests reached the fake api.github.com"
sshv 'pkill -f "[w]e-manager" 2>/dev/null; for _ in $(seq 20); do pgrep -f "[w]e-manager" >/dev/null || break; sleep 1; done; sudo rm -f /etc/polkit-1/rules.d/49-lab-test.rules; sudo sed -i "/api.github.com/d" /etc/hosts; true'
exit $status
