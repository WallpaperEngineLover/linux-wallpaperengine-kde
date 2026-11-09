#!/bin/bash
# coverage_flatpak.sh <vm> - the engine of a coverage candidate (~/lwe/lwe-hints in the guest, release_check.sh --candidate)
# run inside the Flatpak sandbox of we_manager, which is what makes the compositor's layer shell invisible on Hyprland and
# the engine fetch its Wayland connection from the host (Desktop/HostWayland). Needs FLATPAK_BUNDLE (a we_manager .flatpak, only
# its sandbox and runtime matter, the engine inside is replaced) and a running session (sessenv). Used as
# COVERAGE_STEP by release_check.sh, the profile data of the sandboxed engine goes to $COLLECT_GCOV/<vm>-flatpak.
# Prints one PASS|FAIL line per step.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1
app=io.github.WallpaperEngineLover.we_manager
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
[ -f "${FLATPAK_BUNDLE:-}" ] || { echo "FAIL no FLATPAK_BUNDLE"; exit 1; }

sshv 'mkdir -p ~/wem'
"$HERE/linuxvm.sh" scp "$vm" "$FLATPAK_BUNDLE" wem/app.flatpak
sshv "flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo; flatpak --user install -y --noninteractive --reinstall ~/wem/app.flatpak >/tmp/fp-install.log 2>&1" \
    && echo "PASS the bundle installs" || { echo "FAIL install: $(sshv 'tail -n 3 /tmp/fp-install.log')"; exit 1; }

files=$(sshv "flatpak --user info -l $app")/files
engine=$files/lib/linux-wallpaperengine-kde
sshv "rm -rf '$engine' && cp -a ~/lwe/lwe-hints '$engine' && sed -i 's|^export GCOV_PREFIX=.*|export GCOV_PREFIX=\"\$HOME/.var/app/$app/gcov\" GCOV_PREFIX_STRIP=0|' '$engine/linux-wallpaperengine'" \
    && echo "PASS the coverage engine is inside the Flatpak" || { echo "FAIL could not replace the engine"; exit 1; }
sshv 'rm -rf ~/.var/app/'$app'/gcov; mkdir -p ~/.var/app/'$app'/gcov'

run_engine() {
    local label=$1 seconds=$2
    shift 2
    sshv ". ~/sessenv; (setsid nohup flatpak run --command=/app/lib/linux-wallpaperengine-kde/linux-wallpaperengine $app --assets-dir \$HOME/lwe/assets --screen-root \${SCREEN:-Virtual-1} --silent $* > /tmp/fp-$label.log 2>&1 </dev/null &); sleep $seconds"
    local pids
    pids=$(sshv 'pgrep -f "/linux-wallpaperengine[.]bin"' | wc -l)
    sshv 'pkill -TERM -f "/linux-wallpaperengine[.]bin"; sleep 5; pkill -KILL -f "/linux-wallpaperengine[.]bin"; true'
    if [ "$pids" -gt 0 ]; then echo "PASS $label: the sandboxed engine ran"; else echo "FAIL $label: no engine process: $(sshv "tail -n 3 /tmp/fp-$label.log")"; fi
    sshv "grep -m1 -i 'helper\|layer shell\|Wayland' /tmp/fp-$label.log | cut -c1-150"
}
run_engine scene 40 '$HOME/lwe/wp-1120596121'
run_engine web 60 '$HOME/lwe/wp-1078208425'
run_engine puppet 30 '$HOME/lwe/wp-3471294034'

if [ -n "${COLLECT_GCOV:-}" ]; then
    mkdir -p "$COLLECT_GCOV/$vm-flatpak"
    sshv "tar -C ~/.var/app/$app/gcov -cf - . 2>/dev/null" | tar -C "$COLLECT_GCOV/$vm-flatpak" -xf -
fi
