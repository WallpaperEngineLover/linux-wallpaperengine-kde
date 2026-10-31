#!/bin/bash
# desktop_icons_matrix.sh [--lanes N] [--log file] [vm[:WxH]]... - desktop_icons_check.sh over VMs, each from its golden state,
# LANES (default 4) at a time, VMs powered off afterwards. Without arguments: every profile, fedora-kde also at 1280x800, 2560x1440
# and 3840x2160. The profile gives what the release gate does to get a session (LOGIN_PASSWORD at a greeter, PREP, a second
# monitor is not used). Per VM log in <log dir>/<vm>-<size>.log, the merged result in the log file.
# CAND is a folder with lwe-hints/ (the engine under test) copied over the guest's ~/lwe/lwe-hints, as release_check.sh does.
# Slots: at most MAXVMS (default 5) VMs run at once over every copy of this script (a flock per slot in /tmp/lwe-vm-slots), the
# queue itself takes LANES of them. A re-run of a VM that failed because of the harness is just another invocation with that VM
# (`desktop_icons_matrix.sh --log x.log vm`): it takes any free slot, the fifth or one the queue left behind when it ran dry.
HERE=$(cd "$(dirname "$0")" && pwd)
lanes=${LANES:-4} log=${XDG_CACHE_HOME:-$HOME/.cache}/lwe-release-prep/desktop-icons/matrix.log
list=()
while [ $# -gt 0 ]; do
    case $1 in
    --lanes) lanes=$2; shift ;;
    --log) log=$2; shift ;;
    *) list+=("$1") ;;
    esac
    shift
done
if [ ${#list[@]} -eq 0 ]; then
    for f in "$HERE"/profiles/*.sh; do list+=("$(basename "$f" .sh)"); done
    list+=(fedora-kde:1280x800 fedora-kde:2560x1440 fedora-kde:3840x2160)
fi
dir=$(dirname "$log")
mkdir -p "$dir"
. "$HERE/vm_slots.sh"
export HERE dir
export MAXVMS=${MAXVMS:-5} CAND

one() {
    local entry=$1 vm size out
    take_slot
    vm=${entry%%:*}
    size=1920x1080
    [[ $entry == *:* ]] && size=${entry#*:}
    out=$dir/$vm-$size.log
    {
        echo "=== $vm $size $(date +%T)"
        unset SCREEN LOGIN_PASSWORD PREP ICONS
        # shellcheck disable=SC1090
        . "$HERE/profiles/$vm.sh" || { echo "FAIL $vm: no profile"; exit 0; }
        if [ "$ICONS" = 0 ]; then echo "SKIP $vm: ICONS=0 in the profile"; exit 0; fi
        "$HERE/linuxvm.sh" fresh "$vm" >/dev/null 2>&1
        LINUXVM_SIZE=$size "$HERE/linuxvm.sh" start "$vm" >/dev/null 2>&1
        local logged_in=0
        for _ in $(seq 60); do
            if [ -n "$LOGIN_PASSWORD" ] && [ $logged_in = 0 ] && "$HERE/linuxvm.sh" ssh "$vm" true </dev/null 2>/dev/null; then
                sleep 25; "$HERE/linuxvm.sh" type "$vm" "$LOGIN_PASSWORD"; "$HERE/linuxvm.sh" keys "$vm" ret; logged_in=1
            fi
            "$HERE/linuxvm.sh" scp "$vm" "$HERE/sessenv.sh" lwe/sessenv.sh 2>/dev/null \
                && "$HERE/linuxvm.sh" ssh "$vm" "bash ~/lwe/sessenv.sh" </dev/null >/dev/null 2>&1 && break
            sleep 5
        done
        [ -n "$PREP" ] && "$HERE/linuxvm.sh" ssh "$vm" ". ~/sessenv; $PREP" </dev/null
        if [ -n "$CAND" ]; then
            "$HERE/linuxvm.sh" ssh "$vm" "rm -rf ~/lwe/lwe-hints.new" </dev/null
            "$HERE/linuxvm.sh" scp "$vm" "$CAND/lwe-hints" lwe/lwe-hints.new \
                && "$HERE/linuxvm.sh" ssh "$vm" "rm -rf ~/lwe/lwe-hints && mv ~/lwe/lwe-hints.new ~/lwe/lwe-hints" </dev/null
        fi
        sleep 20
        timeout 900 "$HERE/desktop_icons_check.sh" "$vm" "${SCREEN:-Virtual-1}" 2>&1
        "$HERE/linuxvm.sh" ssh "$vm" 'sudo systemctl poweroff' </dev/null >/dev/null 2>&1
        sleep 15
        "$HERE/linuxvm.sh" kill "$vm" >/dev/null 2>&1
    } > "$out" 2>&1
}
export -f one take_slot

printf '%s\n' "${list[@]}" | xargs -P "$lanes" -I{} bash -c 'one {}'
: > "$log"
for entry in "${list[@]}"; do
    vm=${entry%%:*}; size=1920x1080; [[ $entry == *:* ]] && size=${entry#*:}
    grep -hv '^     screenshots' "$dir/$vm-$size.log" >> "$log"
done
echo DONE >> "$log"
grep -c '^FAIL' "$log" | sed 's/^/failed: /'
