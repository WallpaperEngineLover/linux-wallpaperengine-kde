#!/bin/bash
# release_check.sh [--vm name]... [--candidate dir] [--app dir|.deb] [--no-overlay] - the release gate of
# RELEASE_TESTING.md: every VM with a profile (a lane runs its VMs one after the other, the gate uses 4 lanes with `--vm`; every VM takes one of MAXVMS=5 slots from vm_slots.sh first), each from its golden disk (a throwaway overlay, see
# `linuxvm.sh golden`), with guest_test.sh's checks (wallpapers, hotswap, pause on fullscreen, SIGKILL cleanup), the
# frame checks, the desktop icons check (desktop_icons_check.sh, we_manager's overlay on Plasma), we_manager's install/launch/autostart (wem_check.sh) and update flow (update_check.sh). A failed check
# is retried once. Writes reports/<date>/summary.md plus the screenshots and logs of every VM, exit code 1 when anything
# failed. VMs are powered off afterwards, the overlay is thrown away (KEEP_VM=1 leaves the VM running to look at it).
#   --vm name        only this profile (repeatable); default: every file in profiles/
#   --candidate dir  folder with lwe-hints/ (the engine under test) copied over ~/lwe/lwe-hints in the guest
#   --app path       we_manager's dist/linux-unpacked or a .deb; without it the we_manager checks are skipped
#   --engine-tarball the candidate's portable tarball, served to we_manager as the new engine release (update_check.sh)
#   --no-overlay     run on the VM's disk as it is (a VM without a golden disk is run like this anyway)
#   --quick          for daily runs: the engine level groups (video, automute) only on profiles with FULL_ENGINE=1, the hotswap
#                    cycle only its first 3 swaps elsewhere; the release candidate gets the full run
# A profile (profiles/<vm>.sh) sets: VM, SCREEN (as --screen-root names it), SHOT_CMD (command with $SHOT_FILE, or
# host), FULLSCREEN_CMD / MAXIMIZE_CMD (open a fullscreen / maximized window), WEM (deb | app | none, how we_manager is installed), WEM_FLAGS,
# WEM_LAUNCH (prefix of the we_manager start line, e.g. `systemd-run --user --scope -q` where an ssh session's end kills the app), LOGIN_PASSWORD (typed at a greeter without autologin), BOOT_PREP (guest command run as soon as ssh answers, before the session is awaited: Zorin's Xorg profile switches GDM there), PREP (guest command run once the session is up, e.g. keeps the screen from locking), EXTRA (function name run last,
# gets the VM name; prints PASS/FAIL lines).
HERE=$(cd "$(dirname "$0")" && pwd)
LINUXVM=${LINUXVM:-$HOME/.local/share/linuxvm}
vms=() candidate="" app="" overlay=1 tarball="" quick=0
while [ $# -gt 0 ]; do
    case $1 in
    --vm) vms+=("$2"); shift ;;
    --candidate) candidate=$2; shift ;;
    --app) app=$2; shift ;;
    --engine-tarball) tarball=$2; shift ;;
    --no-overlay) overlay=0 ;;
    --quick) quick=1 ;;
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

. "$HERE/vm_slots.sh"

lv() { "$HERE/linuxvm.sh" "$@"; }
sshv() { lv ssh "$VM" "$@" </dev/null; }

wait_off() {
    for _ in $(seq 45); do
        [ -f "$LINUXVM/vms/$VM/qemu.pid" ] && kill -0 "$(cat "$LINUXVM/vms/$VM/qemu.pid")" 2>/dev/null || return 0
        sleep 2
    done
    lv kill "$VM"; sleep 2
}

# the guest_test.sh group a result line belongs to, empty when unknown (the whole run is repeated then)
group_of() {
    case $1 in
    "TIME "*) set -- $1; echo "$2" ;;
    *"hotswap cycle"*|*"/shot-cycle-"*) echo cycle ;;
    *"hotswap video"*) echo video ;;
    *"multi monitor"*|*"first monitor"*|*"second monitor"*) echo multi ;;
    *"automute ignore"*) echo automuteignore ;;
    *transition*) echo transition ;;
    *automute*) echo automute ;;
    *"behavior:"*|*"hotswap:"*|*"fullscreen pause"*|*"maximized pause"*|*SIGKILL*|*"/hot-before.png"*|*"/hot-after.png"*) echo behavior ;;
    *"baseline screenshot"*|*"/shot-"[0-9]*".png"*) echo wallpapers ;;
    *) if [[ $1 =~ ^(PASS|FAIL|SKIP)\ [0-9]{8,10}: ]]; then echo wallpapers; fi ;;
    esac
}

tmark() { echo "$1 $((SECONDS - T_LAST))" >> "$report/$VM/timing.txt"; T_LAST=$SECONDS; }

runs() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }

run_checks() {
    local out=/tmp/lwe-test
    lv scp "$VM" "$HERE/guest_test.sh" lwe/guest_test.sh
    lv scp "$VM" "$HERE/fullscreen_window.py" lwe/fullscreen_window.py
    lv scp "$VM" "$HERE/maximize_kde.sh" lwe/maximize_kde.sh
    [ -n "$HIDE" ] && lv scp "$VM" "$HERE/hide_steam_kde.sh" lwe/hide_steam_kde.sh
    sshv "rm -rf $out; mkdir -p $out"
    local shots=""
    if [ "$SHOT_CMD" = host ]; then "$HERE/host_shots.sh" "$VM" "$out" 900 >/dev/null 2>&1 & shots=$!; fi
    sshv ". ~/sessenv; BEHAVIOR=1 AUTOMUTE_IGNORE='${AUTOMUTE_IGNORE:-}' CYCLE_UPTO='${CYCLE_UPTO:-}' ${RETRY_WALLPAPERS:+WALLPAPERS='$RETRY_WALLPAPERS'} ONLY='$ONLY' SETTLE=${SETTLE:-25} LWE_ARGS='--screen-root $SCREEN' SHOT_CMD='$SHOT_CMD' FULLSCREEN_CMD='$FULLSCREEN_CMD' MAXIMIZE_CMD='$MAXIMIZE_CMD' MINIMIZE_CMD='$MINIMIZE_CMD' MULTI_SHOT_CMD='$MULTI_SHOT_CMD' ~/lwe/guest_test.sh"
    [ -n "$shots" ] && wait "$shots"
    local local_dir=$report/$VM/shots
    mkdir -p "$local_dir"
    scp -q -r -i "$LINUXVM/id_ed25519" -P "$(cat "$LINUXVM/vms/$VM/port")" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "lab@127.0.0.1:$out/*" "$local_dir/" 2>/dev/null
    local shot
    if [ -f "$local_dir/baseline.png" ]; then
        if runs wallpapers; then
            for shot in "$local_dir"/shot-[0-9]*.png; do [ -f "$shot" ] && python3 -I "$HERE/check_frame.py" --baseline "$local_dir/baseline.png" "$shot"; done
        fi
        if runs cycle; then
            for shot in "$local_dir"/shot-cycle-*.png; do [ -f "$shot" ] && python3 -I "$HERE/check_frame.py" --baseline "$local_dir/baseline.png" "$shot"; done
        fi
        if runs windowbg; then
            for shot in "$local_dir"/shot-windowbg-*.png; do [ -f "$shot" ] && python3 -I "$HERE/check_frame.py" --baseline "$local_dir/baseline.png" "$shot"; done
        fi
        if runs wallpapers || runs cycle || runs windowbg; then
            local id
            for shot in "$local_dir"/shot-[0-9]*.png "$local_dir"/shot-cycle-*.png "$local_dir"/shot-windowbg-*.png; do
                [ -f "$shot" ] || continue
                id=${shot##*-}; id=${id%.png}
                [ -f "$HERE/reference/$id.png" ] && python3 -I "$HERE/check_frame.py" --reference "$HERE/reference/$id.png" "$shot"
            done
        fi
    fi
    runs video && [ -f "$local_dir/fresh-video.png" ] && [ -f "$local_dir/swap-video.png" ] \
        && python3 -I "$HERE/check_frame.py" --similar "$local_dir/fresh-video.png" "$local_dir/swap-video.png" 14 \
        | sed 's/^\(PASS\|FAIL\) /\1 hotswap video: /'
    if runs multi && [ -f "$local_dir/multi-geometry" ] && [ -f "$local_dir/multi-shot.png" ]; then
        read -r w1 w2 < "$local_dir/multi-geometry"
        split=$(python3 -c "print($w1 / ($w1 + $w2))")
        python3 -I "$HERE/check_frame.py" --crop "0,$split" --baseline "$local_dir/multi-baseline.png" "$local_dir/multi-shot.png" \
            | sed 's/^\(PASS\|FAIL\) /\1 first monitor: /'
        python3 -I "$HERE/check_frame.py" --crop "$split,1" --baseline "$local_dir/multi-baseline.png" "$local_dir/multi-shot.png" \
            | sed 's/^\(PASS\|FAIL\) /\1 second monitor: /'
    fi
    runs transition && [ -f "$local_dir/tr-half.ppm" ] && [ -f "$local_dir/tr-half-old.ppm" ] && [ -f "$local_dir/tr-half-new.ppm" ] \
        && python3 -I "$HERE/check_frame.py" --blend "$local_dir/tr-half-old.ppm" "$local_dir/tr-half-new.ppm" "$local_dir/tr-half.ppm" \
        | sed 's/^\(PASS\|FAIL\|SKIP\) /\1 transition: /'
    runs behavior && [ -f "$local_dir/hot-before.png" ] && python3 -I "$HERE/check_frame.py" --differ "$local_dir/hot-before.png" "$local_dir/hot-after.png"
    return 0
}

for VM in "${vms[@]}"; do
    take_slot
    unset AUTOMUTE_IGNORE FULL_ENGINE HIDE POINTER POINTER_DOCK_XY WEM_FLAGS WEM_LAUNCH LOGIN_PASSWORD SCREEN SHOT_CMD FULLSCREEN_CMD MAXIMIZE_CMD MINIMIZE_CMD MULTI MULTI_SHOT_CMD WEM WEM_FLAGS PREP BOOT_PREP EXTRA SETTLE ICONS
    # shellcheck disable=SC1090
    . "$HERE/profiles/$VM.sh" || { echo "no profile for $VM"; release_slot; continue; }
    export WEM_FLAGS WEM_LAUNCH   # the we_manager scripts add them to its command line (COSMIC)
    mkdir -p "$report/$VM"
    log=$report/$VM/run.log
    echo "== $VM $(date +%T)"
    : > "$report/$VM/timing.txt"; T_START=$SECONDS; T_LAST=$SECONDS
    {
        if [ "$overlay" = 1 ] && [ -f "$LINUXVM/vms/$VM/golden.qcow2" ]; then lv fresh "$VM"; fi
        LINUXVM_OUTPUTS=${MULTI:+2} lv start "$VM"
        ready=0 logged_in=0
        if [ -n "$BOOT_PREP" ]; then
            for _ in $(seq 60); do sshv true 2>/dev/null && break; sleep 3; done
            sshv "$BOOT_PREP"
        fi
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
            [ -n "$HIDE" ] && lv scp "$VM" "$HERE/hide_steam_kde.sh" lwe/hide_steam_kde.sh
            [ -n "$PREP" ] && sshv ". ~/sessenv; $PREP"
            if [ -n "$candidate" ]; then
                sshv "rm -rf ~/lwe/lwe-hints.new"
                lv scp "$VM" "$candidate/lwe-hints" lwe/lwe-hints.new && sshv "rm -rf ~/lwe/lwe-hints && mv ~/lwe/lwe-hints.new ~/lwe/lwe-hints"
            fi
            for wp in wp-3471294034 wp-3818199837 wp-1367456719; do
                sshv "[ -d ~/lwe/$wp ]" || lv scp "$VM" "$LINUXVM/payload/$wp" lwe/
            done
            sleep 20
            sshv ". ~/sessenv; pkill -x we-manager; sleep 3; pkill -x we-manager; true"
            if [ -n "$HIDE" ]; then lv scp "$VM" "$HERE/hide_steam_kde.sh" lwe/hide_steam_kde.sh; sshv ". ~/sessenv; bash ~/lwe/hide_steam_kde.sh"; fi
            tmark boot
            first_only="" CYCLE_UPTO=""
            if [ $quick = 1 ] && [ "${FULL_ENGINE:-0}" != 1 ]; then first_only="wallpapers behavior cycle transition multi windowbg"; CYCLE_UPTO=3; fi
            ONLY="$first_only" run_checks > "$report/$VM/checks.txt" 2>&1
            CYCLE_UPTO=""
            tmark checks
            fatal=0
            if grep -q '^FATAL' "$report/$VM/checks.txt"; then
                fatal=1
                echo "FATAL, the rest of the run for $VM is skipped"
            elif grep -q '^FAIL' "$report/$VM/checks.txt"; then
                retry="" full=0 RETRY_WALLPAPERS="" cycle_upto=0
                while IFS= read -r line; do
                    g=$(group_of "$line")
                    if [ -z "$g" ]; then full=1; elif [[ " $retry " != *" $g "* ]]; then retry="$retry $g"; fi
                    case $g in
                    wallpapers)
                        id=$(grep -oE '(shot-|^FAIL )[0-9]{8,10}' <<< "$line" | grep -oE '[0-9]{8,10}' | head -1)
                        if [ -z "$id" ]; then RETRY_WALLPAPERS=all; elif [ "$RETRY_WALLPAPERS" != all ] && [[ " $RETRY_WALLPAPERS " != *" $id "* ]]; then RETRY_WALLPAPERS="$RETRY_WALLPAPERS $id"; fi ;;
                    cycle)
                        step=$(grep -oE '(shot-cycle-|hotswap cycle step )[0-9]+' <<< "$line" | grep -oE '[0-9]+$' | head -1)
                        if [ -z "$step" ]; then cycle_upto=99; elif [ "$step" -gt "$cycle_upto" ]; then cycle_upto=$step; fi ;;
                    esac
                done < <(grep '^FAIL' "$report/$VM/checks.txt")
                [ $full = 1 ] && retry=""
                [ "$RETRY_WALLPAPERS" = all ] && RETRY_WALLPAPERS=""
                RETRY_WALLPAPERS=${RETRY_WALLPAPERS# }
                CYCLE_UPTO=""; [ "$cycle_upto" -gt 0 ] && [ "$cycle_upto" -lt 99 ] && CYCLE_UPTO=$cycle_upto
                [ -n "$retry" ] && [[ " $retry " != *" wallpapers "* ]] && RETRY_WALLPAPERS=""
                echo "retrying the failed checks once (groups:${retry:- all}${RETRY_WALLPAPERS:+, wallpapers $RETRY_WALLPAPERS}${CYCLE_UPTO:+, cycle up to step $CYCLE_UPTO})"
                sshv "pkill -f '[l]inux-wallpaperengine' 2>/dev/null; true"; sleep 5
                cp "$report/$VM/checks.txt" "$report/$VM/checks-first.txt"
                ONLY="${retry# }" run_checks > "$report/$VM/checks-retry.txt" 2>&1
                if [ -z "$retry" ]; then cp "$report/$VM/checks-retry.txt" "$report/$VM/checks.txt"; else
                    while IFS= read -r line; do
                        g=$(group_of "$line")
                        if [ -n "$g" ] && [[ " $retry " == *" $g "* ]]; then
                            replaced=1
                            case $g in
                            wallpapers)
                                if [ -n "$RETRY_WALLPAPERS" ] && [[ $line != TIME* ]]; then
                                    replaced=0
                                    for id in $RETRY_WALLPAPERS; do [[ $line == *"$id"* ]] && replaced=1; done
                                fi ;;
                            cycle)
                                if [ -n "$CYCLE_UPTO" ] && [[ $line != TIME* ]]; then
                                    step=$(grep -oE '(shot-cycle-|hotswap cycle step )[0-9]+' <<< "$line" | grep -oE '[0-9]+$' | head -1)
                                    [ -n "$step" ] && [ "$step" -gt "$CYCLE_UPTO" ] && replaced=0
                                fi ;;
                            esac
                            [ $replaced = 1 ] && continue
                        fi
                        echo "$line"
                    done < "$report/$VM/checks-first.txt" > "$report/$VM/checks.txt"
                    cat "$report/$VM/checks-retry.txt" >> "$report/$VM/checks.txt"
                    awk '!seen[$0]++' "$report/$VM/checks.txt" > "$report/$VM/checks.merged" && mv "$report/$VM/checks.merged" "$report/$VM/checks.txt"
                fi
                RETRY_WALLPAPERS="" CYCLE_UPTO=""
            fi
            [ -f "$report/$VM/checks-retry.txt" ] && tmark retry
            if [ $fatal = 0 ] && [ "${POINTER:-1}" = 1 ]; then
                POINTER_DOCK_XY=${POINTER_DOCK_XY:-} "$HERE/pointer_check.sh" "$VM" "$SCREEN" "$report/$VM/shots" > "$report/$VM/pointer.txt" 2>&1
                if grep -q '^FAIL' "$report/$VM/pointer.txt"; then
                    cp "$report/$VM/pointer.txt" "$report/$VM/pointer-first.txt"
                    POINTER_DOCK_XY=${POINTER_DOCK_XY:-} "$HERE/pointer_check.sh" "$VM" "$SCREEN" "$report/$VM/shots" > "$report/$VM/pointer.txt" 2>&1
                fi
                grep -E '^(PASS|FAIL|SKIP)' "$report/$VM/pointer.txt" >> "$report/$VM/checks.txt"
            fi
            tmark pointer
            if [ $fatal = 0 ] && [ "${ICONS:-1}" = 1 ]; then
                sshv "pkill -f '[l]inux-wallpaperengine' 2>/dev/null; true"; sleep 3
                "$HERE/desktop_icons_check.sh" "$VM" "$SCREEN" > "$report/$VM/icons.txt" 2>&1
                if grep -q '^FAIL' "$report/$VM/icons.txt"; then
                    cp "$report/$VM/icons.txt" "$report/$VM/icons-first.txt"
                    "$HERE/desktop_icons_check.sh" "$VM" "$SCREEN" > "$report/$VM/icons.txt" 2>&1
                fi
                grep -E '^(PASS|FAIL|SKIP)' "$report/$VM/icons.txt" >> "$report/$VM/checks.txt"
            fi
            tmark icons
            if [ $fatal = 0 ] && [ -n "$app" ] && [ "${WEM:-none}" != none ]; then
                # with the candidate's engine tarball the update check installs it (as a v99 release), we_manager's own
                # checks then run on it instead of the last published engine
                {
                    [ -n "$tarball" ] && ENGINE_TARBALL=$tarball "$HERE/clean_install_check.sh" "$VM" "$app"
                    ENGINE_TARBALL=$tarball "$HERE/update_check.sh" "$VM" "$app"
                    SKIP_ENGINE_INSTALL=${tarball:+1} "$HERE/wem_check.sh" "$VM" "$app" "$SCREEN"
                } > "$report/$VM/wem.txt" 2>&1
            fi
            tmark wem
            [ $fatal = 0 ] && [ -n "$EXTRA" ] && $EXTRA "$VM" > "$report/$VM/extra.txt" 2>&1
            tmark extra
            sshv "cat /tmp/lwe-test/engine-*.log 2>/dev/null | tail -n 200" > "$report/$VM/engine-tail.log" 2>&1
        fi
        if [ -n "$KEEP_VM" ]; then echo "KEEP_VM set: $VM stays up for a look"; else
            sshv "sudo systemctl poweroff" >/dev/null 2>&1 || lv stop "$VM" >/dev/null 2>&1; wait_off
            [ "$overlay" = 1 ] && [ -f "$LINUXVM/vms/$VM/golden.qcow2" ] && lv fresh "$VM"
        fi
        tmark shutdown
    } > "$log" 2>&1
    pass=$(cat "$report/$VM"/{checks,wem,extra}.txt 2>/dev/null | grep -c '^PASS')
    failed=$(cat "$report/$VM"/{checks,wem,extra}.txt 2>/dev/null | grep -E '^(FAIL|FATAL)'; grep -E '^(FAIL|FATAL)' "$log")
    {
        echo; echo "## $VM"
        if [ -z "$failed" ]; then echo "$pass checks passed."; else
            echo "$pass checks passed, failed:"; echo; echo "$failed" | sed 's/^/- /'; total_status=1
        fi
        for f in "$report/$VM"/{checks,wem,extra}.txt; do grep -h '^SKIP' "$f" 2>/dev/null | sed 's/^/- /'; done
        echo "- time $((SECONDS - T_START)) s: $(awk '{printf "%s%s %d", (NR > 1 ? ", " : ""), $1, $2}' "$report/$VM/timing.txt"); groups: $(grep -h '^TIME' "$report/$VM/checks.txt" 2>/dev/null | awk '{printf "%s%s %d", (NR > 1 ? ", " : ""), $2, $3}')"
    } >> "$summary"
    echo "   $pass passed$([ -n "$failed" ] && echo ", FAILED")"
    release_slot
done
echo "report: $summary"
exit $total_status
