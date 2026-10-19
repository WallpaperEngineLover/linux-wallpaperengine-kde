#!/bin/bash
# release_check.sh [--vm name]... [--candidate dir] [--app dir|.deb] [--no-overlay] - the release gate of
# RELEASE_TESTING.md: every VM with a profile, one after the other, each from its golden disk (a throwaway overlay, see
# `linuxvm.sh golden`), with guest_test.sh's checks (wallpapers, hotswap, pause on fullscreen, SIGKILL cleanup), the
# frame checks, we_manager's install/launch/autostart (wem_check.sh) and update flow (update_check.sh). A failed check
# is retried once. Writes reports/<date>/summary.md plus the screenshots and logs of every VM, exit code 1 when anything
# failed. VMs are powered off afterwards, the overlay is thrown away (KEEP_VM=1 leaves the VM running to look at it).
#   --vm name        only this profile (repeatable); default: every file in profiles/
#   --candidate dir  folder with lwe-hints/ (the engine under test) copied over ~/lwe/lwe-hints in the guest
#   --app path       we_manager's dist/linux-unpacked or a .deb; without it the we_manager checks are skipped
#   --engine-tarball the candidate's portable tarball, served to we_manager as the new engine release (update_check.sh)
#   --no-overlay     run on the VM's disk as it is (a VM without a golden disk is run like this anyway)
# A profile (profiles/<vm>.sh) sets: VM, SCREEN (as --screen-root names it), SHOT_CMD (command with $SHOT_FILE, or
# host), FULLSCREEN_CMD / MAXIMIZE_CMD (open a fullscreen / maximized window), WEM (deb | app | none, how we_manager is installed), WEM_FLAGS,
# LOGIN_PASSWORD (typed at a greeter without autologin), PREP (guest command run once the session is up, e.g. keeps the screen from locking), EXTRA (function name run last,
# gets the VM name; prints PASS/FAIL lines).
HERE=$(cd "$(dirname "$0")" && pwd)
LINUXVM=${LINUXVM:-$HOME/.local/share/linuxvm}
vms=() candidate="" app="" overlay=1 tarball=""
while [ $# -gt 0 ]; do
    case $1 in
    --vm) vms+=("$2"); shift ;;
    --candidate) candidate=$2; shift ;;
    --app) app=$2; shift ;;
    --engine-tarball) tarball=$2; shift ;;
    --no-overlay) overlay=0 ;;
    *) sed -n '2,20p' "$0"; exit 2 ;;
    esac
    shift
done
[ ${#vms[@]} -gt 0 ] || for f in "$HERE"/profiles/*.sh; do vms+=("$(basename "$f" .sh)"); done

report=${REPORT_DIR:-$HERE/reports/$(date +%F)}
mkdir -p "$report"
summary=$report/summary.md
echo "# Release check $(date '+%F %H:%M')" > "$summary"
[ -n "$candidate" ] && echo "Engine candidate: \`$candidate\`" >> "$summary"
[ -n "$app" ] && echo "we_manager: \`$app\`" >> "$summary"
total_status=0

lv() { "$HERE/linuxvm.sh" "$@"; }
sshv() { lv ssh "$VM" "$@" </dev/null; }

wait_off() {
    for _ in $(seq 45); do
        [ -f "$LINUXVM/vms/$VM/qemu.pid" ] && kill -0 "$(cat "$LINUXVM/vms/$VM/qemu.pid")" 2>/dev/null || return 0
        sleep 2
    done
    lv kill "$VM"; sleep 2
}

# run the checks of one VM, results as PASS/FAIL/SKIP lines on stdout
run_checks() {
    local out=/tmp/lwe-test
    lv scp "$VM" "$HERE/guest_test.sh" lwe/guest_test.sh
    lv scp "$VM" "$HERE/fullscreen_window.py" lwe/fullscreen_window.py
    lv scp "$VM" "$HERE/maximize_kde.sh" lwe/maximize_kde.sh
    sshv "rm -rf $out; mkdir -p $out"
    local shots=""
    if [ "$SHOT_CMD" = host ]; then "$HERE/host_shots.sh" "$VM" "$out" 900 >/dev/null 2>&1 & shots=$!; fi
    sshv ". ~/sessenv; BEHAVIOR=1 SETTLE=${SETTLE:-25} LWE_ARGS='--screen-root $SCREEN' SHOT_CMD='$SHOT_CMD' FULLSCREEN_CMD='$FULLSCREEN_CMD' MAXIMIZE_CMD='$MAXIMIZE_CMD' ~/lwe/guest_test.sh"
    [ -n "$shots" ] && wait "$shots"
    local local_dir=$report/$VM/shots
    mkdir -p "$local_dir"
    scp -q -r -i "$LINUXVM/id_ed25519" -P "$(cat "$LINUXVM/vms/$VM/port")" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "lab@127.0.0.1:$out/*" "$local_dir/" 2>/dev/null
    local shot
    if [ -f "$local_dir/baseline.png" ]; then
        for shot in "$local_dir"/shot-*.png; do [ -f "$shot" ] && python3 -I "$HERE/check_frame.py" --baseline "$local_dir/baseline.png" "$shot"; done
    fi
    [ -f "$local_dir/hot-before.png" ] && python3 -I "$HERE/check_frame.py" --differ "$local_dir/hot-before.png" "$local_dir/hot-after.png"
    return 0
}

for VM in "${vms[@]}"; do
    unset WEM_FLAGS LOGIN_PASSWORD SCREEN SHOT_CMD FULLSCREEN_CMD MAXIMIZE_CMD WEM WEM_FLAGS PREP EXTRA SETTLE
    # shellcheck disable=SC1090
    . "$HERE/profiles/$VM.sh" || { echo "no profile for $VM"; continue; }
    export WEM_FLAGS   # the we_manager scripts add them to its command line (COSMIC)
    mkdir -p "$report/$VM"
    log=$report/$VM/run.log
    echo "== $VM $(date +%T)"
    {
        if [ "$overlay" = 1 ] && [ -f "$LINUXVM/vms/$VM/golden.qcow2" ]; then lv fresh "$VM"; fi
        lv start "$VM"
        ready=0 logged_in=0
        for _ in $(seq 60); do
            # a greeter without autologin (COSMIC): type the password once the VM answers
            if [ -n "$LOGIN_PASSWORD" ] && [ $logged_in = 0 ] && sshv true 2>/dev/null; then
                sleep 25; lv type "$VM" "$LOGIN_PASSWORD"; lv keys "$VM" ret; logged_in=1
            fi
            lv scp "$VM" "$HERE/sessenv.sh" lwe/sessenv.sh 2>/dev/null && sshv "bash ~/lwe/sessenv.sh" >/dev/null 2>&1 && { ready=1; break; }
            sleep 5
        done
        if [ $ready = 0 ]; then echo "FAIL $VM: no desktop session within 5 minutes"; else
            # a golden disk keeps what an earlier run left: we_manager's autostart entry and playlist, stray engines
            sshv "pkill -x we-manager; pkill -x linux-wallpaper; rm -rf ~/.config/autostart/we-manager.desktop ~/.config/we-manager; true"
            [ -n "$PREP" ] && sshv ". ~/sessenv; $PREP"
            if [ -n "$candidate" ]; then
                sshv "rm -rf ~/lwe/lwe-hints.new"
                lv scp "$VM" "$candidate/lwe-hints" lwe/lwe-hints.new && sshv "rm -rf ~/lwe/lwe-hints && mv ~/lwe/lwe-hints.new ~/lwe/lwe-hints"
            fi
            sleep 20
            run_checks > "$report/$VM/checks.txt" 2>&1
            if grep -q '^FAIL' "$report/$VM/checks.txt"; then
                echo "retrying the failed checks once"
                sshv "pkill -f '[l]inux-wallpaperengine' 2>/dev/null; true"; sleep 5
                cp "$report/$VM/checks.txt" "$report/$VM/checks-first.txt"
                run_checks > "$report/$VM/checks.txt" 2>&1
            fi
            if [ -n "$app" ] && [ "${WEM:-none}" != none ]; then
                # with the candidate's engine tarball the update check installs it (as a v99 release), we_manager's own
                # checks then run on it instead of the last published engine
                {
                    ENGINE_TARBALL=$tarball "$HERE/update_check.sh" "$VM" "$app"
                    SKIP_ENGINE_INSTALL=${tarball:+1} "$HERE/wem_check.sh" "$VM" "$app" "$SCREEN"
                } > "$report/$VM/wem.txt" 2>&1
            fi
            [ -n "$EXTRA" ] && $EXTRA "$VM" > "$report/$VM/extra.txt" 2>&1
            sshv "cat /tmp/lwe-test/engine-*.log 2>/dev/null | tail -n 200" > "$report/$VM/engine-tail.log" 2>&1
        fi
        if [ -n "$KEEP_VM" ]; then echo "KEEP_VM set: $VM stays up for a look"; else
            sshv "sudo systemctl poweroff" >/dev/null 2>&1 || lv stop "$VM" >/dev/null 2>&1; wait_off
            [ "$overlay" = 1 ] && [ -f "$LINUXVM/vms/$VM/golden.qcow2" ] && lv fresh "$VM"
        fi
    } > "$log" 2>&1
    pass=$(cat "$report/$VM"/{checks,wem,extra}.txt 2>/dev/null | grep -c '^PASS')
    failed=$(cat "$report/$VM"/{checks,wem,extra}.txt 2>/dev/null | grep '^FAIL'; grep '^FAIL' "$log")
    {
        echo; echo "## $VM"
        if [ -z "$failed" ]; then echo "$pass checks passed."; else
            echo "$pass checks passed, failed:"; echo; echo "$failed" | sed 's/^/- /'; total_status=1
        fi
        for f in "$report/$VM"/{checks,wem,extra}.txt; do grep -h '^SKIP' "$f" 2>/dev/null | sed 's/^/- /'; done
    } >> "$summary"
    echo "   $pass passed$([ -n "$failed" ] && echo ", FAILED")"
done
echo "report: $summary"
exit $total_status
