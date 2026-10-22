#!/bin/bash
# pointer_check.sh <vm> [screen] [shot dir] - which way does the pointer's y go on this VM's output? Runs workshop item
# 3599498791 (its calendar enlarges the weekday under the pointer, hover zones driven by the engine's cursor hit test), puts the
# pointer on the Monday row and on the Sunday row of the calendar and checks the enlarged label with hover_check.py, and with the pointer on the dock and at the top that the dock icons magnify (the scripts' cursor world position). The two
# screenshots stay in <shot dir> (default /tmp/pointer-check-<vm>). Prints PASS|FAIL lines.
# Needs the VM running with the session up (sessenv), the wallpaper and the engine in ~/lwe (prep_guest.sh, release_check.sh),
# a 1920x1080 display, and the pointer reachable through QMP (usb-tablet).
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 screen=${2:-Virtual-1} out=${3:-/tmp/pointer-check-$vm}
mkdir -p "$out"
lv() { "$HERE/linuxvm.sh" "$@"; }
sshv() { lv ssh "$vm" "$@" </dev/null; }
if ! sshv '[ -d ~/lwe/wp-3599498791 ]'; then lv scp "$vm" "${LINUXVM:-$HOME/.local/share/linuxvm}/payload/wp-3599498791" lwe/; fi
sshv 'pkill -f "[l]inux-wallpaperengine.bin"; pkill -x we-manager; true'
sshv 'pkill -f "[x]fce4-display-settings"; pkill -f "[u]pdate-manager"; pkill -f "[r]elease-upgrader"; pkill -f "[u]pdate-notifier"; pkill -x cachyos-hello; pkill -f "[e]os-welcome|[m]x-welcome|[g]aruda-welcome|[m]anjaro-hello|[m]int-welcome|[z]orin-welcome|[p]op-welcome|[m]intupdate|[m]intUpdate|[m]intreport|[m]intwelcome"; true'
# SteamOS starts Steam's sign-in window at login
sshv 'pkill -x steam; pkill -f "[s]teamwebhelper"; true'
sshv ". ~/sessenv; (setsid ${WEM_LAUNCH:-} nohup ~/lwe/lwe-hints/linux-wallpaperengine --assets-dir ~/lwe/assets $LWE_ARGS_EXTRA --screen-root $screen ~/lwe/wp-3599498791 > /tmp/pointer-check.log 2>&1 < /dev/null &); sleep 6"
for _ in $(seq 60); do sshv 'grep -q "drawing at scale" /tmp/pointer-check.log' && break; sleep 2; done
sleep 30
# welcome apps start late on some distributions, once more now that the engine runs
sshv 'pkill -f "[x]fce4-display-settings"; pkill -f "[u]pdate-manager"; pkill -f "[r]elease-upgrader"; pkill -f "[u]pdate-notifier"; pkill -x cachyos-hello; pkill -f "[e]os-welcome|[m]x-welcome|[g]aruda-welcome|[m]anjaro-hello|[m]int-welcome|[z]orin-welcome|[p]op-welcome|[m]intupdate|[m]intUpdate|[m]intreport|[m]intwelcome"; true'
# a welcome window that survives its process name (EndeavourOS): close it by title
sshv '. ~/sessenv; wmctrl -c "Welcome" 2>/dev/null; wmctrl -c "Plugin loading failure" 2>/dev/null; for w in $(xdotool search --name "^Welcome" 2>/dev/null); do xdotool windowclose $w; done; true'
if ! sshv 'pgrep -f "[l]inux-wallpaperengine.bin" >/dev/null'; then echo "FAIL weekday hover: the engine did not stay running on 3599498791"; exit 1; fi
for spot in mon:713,451 sun:714,625 dock:${POINTER_DOCK_XY:-960,1010} top:960,60; do
    name=${spot%%:*} xy=${spot#*:}
    sshv '. ~/sessenv; wmctrl -c "Welcome" 2>/dev/null; wmctrl -c "Plugin loading failure" 2>/dev/null; for w in $(xdotool search --name "^Welcome" 2>/dev/null); do xdotool windowclose $w; done; true'
    lv pointer "$vm" "${xy%,*}" "${xy#*,}" >/dev/null 2>&1
    sleep 1
    lv pointer "$vm" "$(( ${xy%,*} + 2 ))" "$(( ${xy#*,} + 1 ))" >/dev/null 2>&1
    sleep 10
    lv shot "$vm" "$out/hover-$name.png"
done
sshv 'pkill -f "[l]inux-wallpaperengine.bin"; true'
python3 -I "$HERE/hover_check.py" "$out/hover-mon.png" "$out/hover-sun.png" "$out/hover-dock.png" "$out/hover-top.png"
