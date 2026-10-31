#!/bin/bash
# deps_check.sh <vm> <app dir | installed> [source tarball] - we_manager's "install build dependencies" for a source build
# on the VM's package manager (pkexec with a polkit rule for the lab user standing in for the dialog), then the engine's
# CMake configure on the source tarball: it only passes when everything the build needs got installed. SKIPs the
# install on image based systems (we_manager refuses there). Needs the session running (sessenv).
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 app=$2 src=${3:-${TMPDIR:-/tmp}/lwe-src.tgz}
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
cdp() { sshv ". ~/sessenv; cd ~/wem; timeout ${2:-900} timeout ${CDP_TIMEOUT:-90} python3 cdp.py \"$1\" 2>&1 | cut -c1-600"; }

sshv 'mkdir -p ~/wem; pkill -f "[w]e-manager" 2>/dev/null
sudo mkdir -p /etc/polkit-1/rules.d
printf "polkit.addRule(function(action, subject) { if (subject.user == \"lab\") return polkit.Result.YES; });\n" | sudo tee /etc/polkit-1/rules.d/49-lab-test.rules >/dev/null; true'
"$HERE/linuxvm.sh" scp "$vm" "$HERE/cdp.py" wem/cdp.py
if [ -d "$app" ]; then
    rm_old=$(basename "$app"); sshv "rm -rf ~/wem/$rm_old"
    tar -C "$(dirname "$app")" -cf - "$rm_old" | ssh -i "$HOME/.local/share/linuxvm/id_ed25519" -p "$(cat "$HOME/.local/share/linuxvm/vms/$vm/port")" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR lab@127.0.0.1 'mkdir -p ~/wem && tar -C ~/wem -xf -'
fi
if [[ $app == installed ]]; then start="${WEM_LAUNCH:-} we-manager ${WEM_FLAGS:-}"; else start="${WEM_LAUNCH:-} ~/wem/$(basename "$app")/we-manager --no-sandbox ${WEM_FLAGS:-}"; fi
sshv ". ~/sessenv; (setsid nohup $start --remote-debugging-port=9222 > ~/wem/app-deps.log 2>&1 </dev/null &); sleep 25"
distro=$(cdp 'window.electronAPI.lwe.detectDistro()'); echo "     detected distro: $distro"
if sshv 'test -e /run/ostree-booted'; then
    echo "SKIP dependency install: image based system, we_manager refuses it (no source build there, nothing to configure)"
    sshv 'pkill -f "[w]e-manager" 2>/dev/null; sudo rm -f /etc/polkit-1/rules.d/49-lab-test.rules; true'
    exit 0
else
    out=$(cdp 'window.electronAPI.lwe.installDeps()' 1200)
    if echo "$out" | grep -q '"ok": true'; then pass "we_manager installed the build dependencies ($distro)"; else fail "dependency install: $out"; fi
fi
if [ -n "$LIBNODE_PKG" ]; then
    # Arch only has libnode in the AUR, we_manager leaves it to the user: a package built from the AUR recipe stands in
    "$HERE/linuxvm.sh" scp "$vm" "$LIBNODE_PKG" libnode.pkg.tar.zst
    sshv 'sudo pacman -U --noconfirm ~/libnode.pkg.tar.zst >/tmp/libnode.log 2>&1; tail -1 /tmp/libnode.log'
fi
"$HERE/linuxvm.sh" scp "$vm" "$src" lwe-src.tgz
out=$(sshv 'rm -rf ~/src && mkdir ~/src && tar -C ~/src -xzf ~/lwe-src.tgz && cd ~/src && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /tmp/cmake-deps.log 2>&1; echo rc=$?; grep -E "Found V8|CMake Error|Could NOT|not found" /tmp/cmake-deps.log | head -5')
if echo "$out" | grep -q 'Could NOT find V8' && ! sshv 'ls /usr/lib64/libnode.so* /usr/lib/libnode.so* /usr/lib/*/libnode.so* >/dev/null 2>&1'; then
    echo "SKIP engine configure: this release has no Node.js shared library (libnode), a source build is not possible, the prebuilt engine is the way (we_manager says so after the install)"
elif echo "$out" | grep -q '^rc=0'; then pass "the engine configures with those packages: $(echo "$out" | grep "Found V8")"; else fail "cmake configure failed: $out"; fi
sshv 'pkill -f "[w]e-manager" 2>/dev/null; sudo rm -f /etc/polkit-1/rules.d/49-lab-test.rules; true'
exit $status
