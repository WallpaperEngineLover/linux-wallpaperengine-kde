#!/bin/bash
# guest_test.sh - run inside a VM's desktop session by release_check.sh (or by hand): starts each test wallpaper with the
# engine under test, takes a screenshot, checks the engine survived and stopped cleanly. One "PASS|FAIL <check>" line per
# check, screenshots in $OUT. Needs the session environment (sessenv) sourced first.
#   LWE_DIR       folder with lwe-hints/ (the engine), assets/ and wp-<workshop id>/ folders
#   LWE_ARGS      how the engine is told where to draw, e.g. "--screen-root Virtual-1" or "--window 0x0x1920x1080"
#   SHOT_CMD      screenshot command with $SHOT_FILE as the file, e.g. 'gnome-screenshot -f $SHOT_FILE'; the value
#                 host asks the host instead (COSMIC's portal asks for permission on every call): the script creates
#                 $SHOT_FILE.want and waits for $SHOT_FILE, host_shots.sh takes the picture through the VM's display
#   WALLPAPERS    ids to run, default: scene with parallax, scene with particles, two web items
#   SETTLE        seconds to wait for the first frames (default 25)
# resolved: the engine's command line shows the real path (/var/home on Fedora Atomic systems)
LWE_DIR=$(realpath "${LWE_DIR:-$HOME/lwe}")
ENGINE=$LWE_DIR/lwe-hints/linux-wallpaperengine
OUT=${OUT:-/tmp/lwe-test}
WALLPAPERS=${WALLPAPERS-"1111948150 1120596121 1078208425 834237690"}
SETTLE=${SETTLE:-25}
mkdir -p "$OUT"
rm -f "$OUT/done"
status=0

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
engine_pids() { pgrep -f "^$LWE_DIR/lwe-hints/linux-wallpaperengine.[b]in"; }

take_shot() {
    if [ "$SHOT_CMD" = host ]; then
        touch "$1.want"
        for _ in $(seq 120); do [ -s "$1" ] && break; sleep 1; done
    else
        eval "$SHOT_CMD" > "$2" 2>&1
    fi
}

stop_engine() {
    for p in $(engine_pids); do kill "$p"; done
    for _ in $(seq 20); do engine_pids >/dev/null || return 0; sleep 0.5; done
    for p in $(engine_pids); do kill -9 "$p"; done
    return 1
}

stop_engine
# the desktop without a wallpaper, the reference check_frame.py compares the others to
sleep 2
SHOT_FILE=$OUT/baseline.png
rm -f "$SHOT_FILE"
take_shot "$SHOT_FILE" "$OUT/baseline.log"
if [ -s "$SHOT_FILE" ]; then pass "baseline screenshot taken"; else fail "no baseline screenshot"; fi

for id in $WALLPAPERS; do
    dir=$LWE_DIR/wp-$id
    [ -d "$dir" ] || { fail "$id: wallpaper folder missing"; continue; }
    log=$OUT/engine-$id.log
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" $LWE_ARGS "$dir" > "$log" 2>&1 < /dev/null &)
    sleep "$SETTLE"

    if engine_pids >/dev/null; then pass "$id: engine running after ${SETTLE}s"; else fail "$id: engine exited"; fi
    if grep -qiE "segmentation|SIGSEGV|terminate called|Aborted|core dumped" "$log"; then fail "$id: crash text in the log"; else pass "$id: no crash text in the log"; fi

    SHOT_FILE=$OUT/shot-$id.png
    rm -f "$SHOT_FILE"
    take_shot "$SHOT_FILE" "$OUT/shot-$id.log"
    if [ -s "$SHOT_FILE" ]; then pass "$id: screenshot taken"; else fail "$id: no screenshot"; fi

    if stop_engine; then pass "$id: engine stopped on SIGTERM"; else fail "$id: engine needed SIGKILL"; fi
    if pgrep -f "[l]inux-wallpaperengine-web-helper" >/dev/null; then fail "$id: web helper left behind"; else pass "$id: no web helper left behind"; fi
done
# the test window's own command line is the pattern: toolkits start helpers and detach, the shell's pid is not enough
stop_fullscreen_window() {
    [ -n "$STOP_CMD" ] && pkill -f -- "^${STOP_CMD%% *}.*${STOP_CMD#* }" 2>/dev/null
    pkill -f "lwe-fullscreen-test|fullscreen_window.py|maximize_kde.sh|konsole -e sleep" 2>/dev/null
    return 0
}

# wait_for_log <file> <text> <seconds> <lines of it seen before>: true once more lines than that contain the text
wait_for_log() {
    for _ in $(seq "$3"); do
        [ "$(grep -c "$2" "$1")" -gt "$4" ] && return 0
        sleep 1
    done
    return 1
}

# pause_check <label> <command>: a window opened by the command must pause the engine (a "Paused: windows" log line) and
# closing it must resume it. An empty command is skipped.
pause_check() {
    local label=$1 cmd=$2 log=$OUT/engine-behavior.log
    if [ -z "$cmd" ]; then echo "SKIP $label pause: no command for this desktop"; return; fi
    STOP_CMD=$cmd
    stop_fullscreen_window
    local paused_before resumed_before fs_pid
    paused_before=$(grep -c "Paused: windows" "$log"); resumed_before=$(grep -c "Resumed" "$log")
    eval "$cmd" > "$OUT/$label-window.log" 2>&1 &
    fs_pid=$!
    sleep 2
    if [ "$SHOT_CMD" = host ]; then
        touch "$OUT/focus.click"
        for _ in $(seq 10); do [ -e "$OUT/focus.click" ] || break; sleep 1; done
    fi
    if ! kill -0 "$fs_pid" 2>/dev/null; then
        fail "$label pause: the test window did not start ($(head -c 150 "$OUT/$label-window.log" | tr '\n' ' '))"
    elif grep -q "Fullscreen detection not supported" "$log"; then
        echo "SKIP $label pause: the compositor has no window state detection"
    elif wait_for_log "$log" "Paused: windows" 45 "$paused_before"; then
        pass "$label pause: engine paused"
        kill "$fs_pid" 2>/dev/null; stop_fullscreen_window
        if wait_for_log "$log" "Resumed" 45 "$resumed_before"; then pass "$label pause: engine resumed"; else fail "$label pause: no resume after the window closed"; fi
    else
        fail "$label pause: no pause within 45s of a $label window"
        {
            echo "--- state at the failed $label check, $(date +%T)"
            echo "window processes:"; pgrep -fa "konsole|fullscreen_window|maximize_kde" | cut -c1-120
            echo "engine bus names:"; busctl --user list 2>/dev/null | grep -i linuxwallpaper
            echo "kwin scripts left in the runtime dir:"; ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E "^lwe-"
            echo "engine log tail:"; grep -v '^ALSA' "$log" | tail -5 | cut -c1-160
            if command -v gdbus >/dev/null && gdbus introspect --session --dest org.kde.KWin --object-path /Scripting >/dev/null 2>&1; then
                echo "KWin's window list:"
                diag=$(mktemp /tmp/lwe-diag-XXXXXX.js)
                echo 'for (const w of workspace.windowList()) print("LWEDIAG", w.resourceClass, w.caption, "fs=" + w.fullScreen, "max=" + (w.maximizeMode), "min=" + w.minimized, "out=" + (w.output ? w.output.name : "none"), "desktops=" + w.desktops.length, "normal=" + w.normalWindow);' > "$diag"
                id=$(gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.loadScript "$diag" lwe-diag | tr -dc 0-9)
                gdbus call --session --dest org.kde.KWin --object-path "/Scripting/Script$id" --method org.kde.kwin.Script.run >/dev/null 2>&1
                sleep 1
                journalctl --user -b --no-pager 2>/dev/null | grep LWEDIAG | tail -12 | cut -c1-220
                gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.unloadScript lwe-diag >/dev/null 2>&1
                rm -f "$diag"
            fi
        } >> "$OUT/pause-diagnostics.txt" 2>&1
    fi
    kill "$fs_pid" 2>/dev/null; stop_fullscreen_window
    sleep 3
}

# behaviour checks, BEHAVIOR=1: hotswap, pause on fullscreen, SIGKILL leaves no helper
if [ "${BEHAVIOR:-0}" = 1 ]; then
    A=${HOTSWAP_FROM:-1120596121}
    B=${HOTSWAP_TO:-1111948150}
    ctl=$OUT/control
    : > "$ctl"
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" $LWE_ARGS --control-file "$ctl" "$LWE_DIR/wp-$A" > "$OUT/engine-behavior.log" 2>&1 < /dev/null &)
    sleep "$SETTLE"
    pid=$(engine_pids | head -1)
    if [ -z "$pid" ]; then fail "behavior: engine did not start"; else
        SHOT_FILE=$OUT/hot-before.png; rm -f "$SHOT_FILE"; take_shot "$SHOT_FILE" "$OUT/hot-before.log"
        echo "path=$LWE_DIR/wp-$B" > "$ctl"
        kill -USR1 "$pid"
        sleep 15
        if grep -q "Hotswapping wallpaper to" "$OUT/engine-behavior.log"; then pass "hotswap: engine took the new wallpaper"; else fail "hotswap: no hotswap in the log"; fi
        if kill -0 "$pid" 2>/dev/null; then pass "hotswap: same process still running"; else fail "hotswap: engine exited"; fi
        SHOT_FILE=$OUT/hot-after.png; rm -f "$SHOT_FILE"; take_shot "$SHOT_FILE" "$OUT/hot-after.log"

        # back to the first wallpaper so the CPU numbers are of a scene with particles
        echo "path=$LWE_DIR/wp-$A" > "$ctl"; kill -USR1 "$pid"; sleep 12
        pause_check "fullscreen" "$FULLSCREEN_CMD"
        pause_check "maximized" "$MAXIMIZE_CMD"
        stop_engine
    fi

    # SIGKILL: the web host must not outlive the engine
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" $LWE_ARGS "$LWE_DIR/wp-1078208425" > "$OUT/engine-kill.log" 2>&1 < /dev/null &)
    sleep "$SETTLE"
    pid=$(engine_pids | head -1)
    helpers_before=$(pgrep -fc "[l]inux-wallpaperengine-web-helper")
    kill -9 "$pid" 2>/dev/null
    sleep 8
    helpers_after=$(pgrep -fc "[l]inux-wallpaperengine-web-helper")
    if [ "$helpers_before" -gt 0 ] && [ "$helpers_after" -eq 0 ]; then pass "SIGKILL: $helpers_before web host processes ended with the engine"
    elif [ "$helpers_before" -eq 0 ]; then fail "SIGKILL: no web host was running to begin with"
    else fail "SIGKILL: $helpers_after of $helpers_before web host processes left behind"; pkill -f "linux-wallpaperengine-web-helper"; fi
fi

touch "$OUT/done"
exit $status
