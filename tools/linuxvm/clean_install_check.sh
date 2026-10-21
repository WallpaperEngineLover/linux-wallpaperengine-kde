#!/bin/bash
# clean_install_check.sh <vm> <app dir | .deb> - a first install of the prebuilt engine through we_manager the way a new user
# does it: no engine under ~/.local/share/linux-wallpaperengine-kde and one of the system libraries the engine leaves to the
# distribution removed (libvulkan: the engine needs it, Electron does not, so we_manager keeps running; nss would be the
# realistic one but Electron needs it too, and libXxf86vm takes Electron down through Mesa on X11), so the "install missing libraries" step has to run. The
# candidate's portable tarball is served by fake_github.py under every asset name we_manager may ask for. Checks: the library
# is gone before, the install reports installed, the library is back, the engine's own library check finds nothing missing.
# Needs the session running (sessenv) and we_manager on the VM (wem_check.sh / update_check.sh put it there).
#   ENGINE_TARBALL the candidate's portable tarball (required)
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 app=$2
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
cdp() { sshv ". ~/sessenv; cd ~/wem; NODE_EXTRA_CA_CERTS= timeout ${CDP_TIMEOUT:-90} python3 cdp.py \"$1\" 2>&1 | cut -c1-${2:-700}"; }
[ -f "$ENGINE_TARBALL" ] || { echo "ENGINE_TARBALL is not set to the candidate's portable tarball"; exit 2; }

remove_cmd=$(sshv '. /etc/os-release; case " $ID $ID_LIKE " in
  *" arch "*) echo "sudo pacman -Rdd --noconfirm vulkan-icd-loader" ;;
  *linuxmint*) echo "sudo apt-get -o DPkg::Lock::Timeout=180 remove -y libvulkan1" ;;
  *" ubuntu "*|*" debian "*) echo "sudo dpkg --remove --force-depends libvulkan1" ;;
  *" fedora "*) echo "sudo rpm -e --nodeps vulkan-loader" ;;
  *suse*) echo "sudo rpm -e --nodeps libvulkan1" ;;
esac')
# Ubuntu 24.04 gets the distribution build, whose own libraries (freeglut and the rest) are missing on a fresh install: nothing has to be
# removed there, and removing libvulkan1 leaves apt with broken dependents (dpkg) or takes the GNOME session along (apt-get remove)
victim=libvulkan.so.1
if sshv '. /etc/os-release; [ "$ID" = ubuntu ] || [ "$ID" = linuxmint ] || case " $ID_LIKE " in *" ubuntu "*) true;; *) false;; esac'; then victim=libglut.so.3.12; remove_cmd="sudo apt-get remove -y libglut3.12"; fi
if [ -z "$remove_cmd" ]; then echo "SKIP clean install: no way to remove a system library on this distribution"; exit 0; fi
if sshv 'touch /usr/.lwe-rw 2>/dev/null && rm /usr/.lwe-rw' || sshv 'sudo touch /usr/.lwe-rw 2>/dev/null && sudo rm /usr/.lwe-rw'; then :; else
    echo "SKIP clean install: read-only system, libraries cannot be installed here"; exit 0
fi

work=$(mktemp -d)
engine_tag=v98.0.$(date +%s)
for variant in kde generic; do
    for distro in portable ubuntu-24.04 fedora-44; do cp "$ENGINE_TARBALL" "$work/linux-wallpaperengine-$variant-$distro-x86_64.tar.gz"; done
done
assets=$(ls "$work"/*.tar.gz | paste -sd,)
mkdir -p "$work/certs"
. "$HERE/fake_github_lib.sh"
fgh_start "$vm" "$work" --release "WallpaperEngineLover/linux-wallpaperengine-kde=$engine_tag:$assets"
trap 'kill $fake 2>/dev/null; rm -rf "$work"; sshv "pkill -f \"[w]e-manager\" 2>/dev/null; sudo sed -i \"/api.github.com/d\" /etc/hosts; true" >/dev/null 2>&1' EXIT
sleep 3
kill -0 $fake 2>/dev/null || { cat "$work/fake.log"; echo "fake_github did not start (port 443?)"; exit 2; }

sshv 'mkdir -p ~/wem'
if [ -d "$app" ]; then
    tar -C "$(dirname "$app")" -cf - "$(basename "$app")" | ssh -i "$HOME/.local/share/linuxvm/id_ed25519" -p "$(cat "$HOME/.local/share/linuxvm/vms/$vm/port")" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR lab@127.0.0.1 'mkdir -p ~/wem && tar -C ~/wem -xf -'
fi
"$HERE/linuxvm.sh" scp "$vm" "$work/certs/ca.pem" wem/fake-ca.pem
"$HERE/linuxvm.sh" scp "$vm" "$HERE/cdp.py" wem/cdp.py
sshv 'grep -q "api.github.com" /etc/hosts || echo "10.0.2.2 api.github.com" | sudo tee -a /etc/hosts >/dev/null
sudo mkdir -p /etc/polkit-1/rules.d
printf "polkit.addRule(function(action, subject) { if (subject.user == \"lab\") return polkit.Result.YES; });\n" | sudo tee /etc/polkit-1/rules.d/49-lab-test.rules >/dev/null
pkill -f "[w]e-manager" 2>/dev/null; for _ in $(seq 20); do pgrep -f "[w]e-manager" >/dev/null || break; sleep 1; done; rm -rf ~/.local/share/linux-wallpaperengine-kde; true'
sleep 2

if [[ $app == *.deb ]]; then
    "$HERE/linuxvm.sh" scp "$vm" "$app" wem/app.deb
    sshv 'sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-downgrades ./wem/app.deb >/tmp/wem-deb.log 2>&1'
fi
if [[ $app == *.deb ]]; then start="${WEM_LAUNCH:-} we-manager ${WEM_FLAGS:-}"; else start="${WEM_LAUNCH:-} ~/wem/$(basename "$app")/we-manager --no-sandbox ${WEM_FLAGS:-}"; fi
sshv ". ~/sessenv; export NODE_EXTRA_CA_CERTS=\$HOME/wem/fake-ca.pem; (setsid nohup $start --remote-debugging-port=9222 > ~/wem/app-clean.log 2>&1 </dev/null &); for _ in \$(seq 40); do grep -q 'DevTools listening' ~/wem/app-clean.log 2>/dev/null && break; sleep 1; done; sleep 8"
[ "$(sshv 'pgrep -fc "[w]e-manager"')" -gt 0 ] || { fail "clean install: we_manager did not start: $(sshv 'tail -3 ~/wem/app-clean.log')"; exit 1; }

sshv "$remove_cmd" > /tmp/lwe-clean-remove.log 2>&1
if sshv "/sbin/ldconfig -p | grep -q \"$victim (libc6,x86-64)\""; then fail "clean install: $victim is still installed after '$remove_cmd'"; exit 1; fi
pass "clean install: no engine and no $victim on the system"

# installPrebuilt resolves with the engine status either way, the outcome comes as progress events
expr='(async () => { const seen = []; const off = window.electronAPI.on.lweInstallProgress(p => seen.push(p.stage + ": " + p.message)); const status = await window.electronAPI.lwe.installPrebuilt(); off(); return { installed: status.installed, last: seen.slice(-3) }; })()'
out=$(sshv ". ~/sessenv; cd ~/wem; NODE_EXTRA_CA_CERTS=\$HOME/wem/fake-ca.pem timeout 900 python3 cdp.py '$expr' 2>&1 | cut -c1-700")
if echo "$out" | grep -q '"installed": true' && ! echo "$out" | grep -q 'error: '; then pass "clean install: we_manager installed the engine"
else fail "clean install: the install failed: $out"; fi
if [ "$victim" = libglut.so.3.12 ]; then echo "SKIP clean install: the test tarball bundles $victim, nothing has to be installed on the way"
elif sshv "/sbin/ldconfig -p | grep -q \"$victim (libc6,x86-64)\""; then pass "clean install: the missing system library was installed on the way"
else fail "clean install: $victim is still missing after the install"; fi
engine=$(sshv 'find ~/.local/share/linux-wallpaperengine-kde -maxdepth 2 -name linux-wallpaperengine -type f | head -1')
missing=$(sshv "$engine --check-libraries 2>&1 | grep -c 'not found'")
if [ "$missing" = 0 ]; then pass "clean install: the engine finds all its libraries"; else fail "clean install: $missing libraries still not found"; fi
sshv 'pkill -f "[w]e-manager" 2>/dev/null; for _ in $(seq 20); do pgrep -f "[w]e-manager" >/dev/null || break; sleep 1; done; sudo rm -f /etc/polkit-1/rules.d/49-lab-test.rules; true'
exit $status
