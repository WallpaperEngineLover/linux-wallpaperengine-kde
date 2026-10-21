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
#   SETTLE        seconds to wait for the first frames (default 25), the checks that do not compare frames at a fixed time
#                 go on as soon as the engine says it draws (a few seconds more, web pages need longer)
#   ONLY          space separated check groups to run (wallpapers behavior video cycle automute multi), default all
#   MINIMIZE_CMD  minimizes the test window (X11): the engine paused by a fullscreen window must resume once it is hidden
#   MULTI_SHOT_CMD  screenshot command that captures every monitor (the VM was started with a second output): turns on
#                 the multi monitor check, the wallpaper must show on both screens
# resolved: the engine's command line shows the real path (/var/home on Fedora Atomic systems)
LWE_DIR=$(realpath "${LWE_DIR:-$HOME/lwe}")
ENGINE=$LWE_DIR/lwe-hints/linux-wallpaperengine
OUT=${OUT:-/tmp/lwe-test}
WALLPAPERS=${WALLPAPERS-"1111948150 1120596121 1078208425 834237690"}
SETTLE=${SETTLE:-25}
mkdir -p "$OUT"
rm -f "$OUT/done"
status=0

want() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }

settle() {
    local log=$1 id=$2 extra=6 waited=0 rest
    if grep -qi '"type"[[:space:]]*:[[:space:]]*"web"' "$LWE_DIR/wp-$id/project.json" 2>/dev/null; then extra=16; fi
    while [ "$waited" -lt "$SETTLE" ] && ! grep -q "drawing at scale" "$log" 2>/dev/null; do sleep 1; waited=$((waited + 1)); done
    rest=$((SETTLE - waited))
    [ "$rest" -gt "$extra" ] && rest=$extra
    [ "$rest" -gt 0 ] && sleep "$rest"
    return 0
}

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
total=0 bad_start=0
if want wallpapers; then
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
    settle "$log" "$id"

    total=$((total + 1))
    if engine_pids >/dev/null; then pass "$id: engine running"; else fail "$id: engine exited"; bad_start=$((bad_start + 1)); fi
    if grep -qiE "segmentation|SIGSEGV|terminate called|Aborted|core dumped" "$log"; then fail "$id: crash text in the log"; else pass "$id: no crash text in the log"; fi

    SHOT_FILE=$OUT/shot-$id.png
    rm -f "$SHOT_FILE"
    take_shot "$SHOT_FILE" "$OUT/shot-$id.log"
    if [ -s "$SHOT_FILE" ]; then pass "$id: screenshot taken"; else fail "$id: no screenshot"; fi

    if stop_engine; then pass "$id: engine stopped on SIGTERM"; else fail "$id: engine needed SIGKILL"; fi
    if pgrep -f "[l]inux-wallpaperengine-web-helper" >/dev/null; then fail "$id: web helper left behind"; else pass "$id: no web helper left behind"; fi
done
    if [ "$total" -gt 0 ] && [ "$bad_start" -eq "$total" ]; then
        echo "FATAL: the engine did not stay running on any of the $total wallpapers, the other checks are skipped"
        touch "$OUT/done"; exit 1
    fi
fi

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
        local minimized_resumed=0
        if [ "$label" = fullscreen ] && [ -n "$MINIMIZE_CMD" ]; then
            if command -v xdotool >/dev/null; then
                eval "$MINIMIZE_CMD" > "$OUT/minimize.log" 2>&1
                if wait_for_log "$log" "Resumed" 20 "$resumed_before"; then pass "$label pause: engine resumed once the window was minimized"; minimized_resumed=1
                else fail "$label pause: still paused with the window minimized"; fi
                resumed_before=$(grep -c "Resumed" "$log")
            else
                echo "SKIP $label pause: no xdotool to minimize the window"
            fi
        fi
        kill "$fs_pid" 2>/dev/null; stop_fullscreen_window
        if [ $minimized_resumed = 1 ]; then :
        elif wait_for_log "$log" "Resumed" 45 "$resumed_before"; then pass "$label pause: engine resumed"; else fail "$label pause: no resume after the window closed"; fi
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

hotswap_video_check() {
    local A=3471294034 B=3818199837 ctl=$OUT/control-video pid
    if [ ! -d "$LWE_DIR/wp-$A" ] || [ ! -d "$LWE_DIR/wp-$B" ]; then echo "SKIP hotswap video: wallpapers $A and $B missing"; return; fi
    : > "$ctl"
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" $LWE_ARGS --control-file "$ctl" "$LWE_DIR/wp-$A" > "$OUT/engine-swapvideo.log" 2>&1 < /dev/null &)
    sleep "$SETTLE"
    pid=$(engine_pids | head -1)
    if [ -z "$pid" ]; then fail "hotswap video: engine did not start"; return; fi
    echo "path=$LWE_DIR/wp-$B" > "$ctl"
    kill -USR1 "$pid"
    sleep 20
    SHOT_FILE=$OUT/swap-video.png; rm -f "$SHOT_FILE"; take_shot "$SHOT_FILE" "$OUT/swap-video.log"
    stop_engine
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" $LWE_ARGS "$LWE_DIR/wp-$B" > "$OUT/engine-freshvideo.log" 2>&1 < /dev/null &)
    sleep "$SETTLE"
    SHOT_FILE=$OUT/fresh-video.png; rm -f "$SHOT_FILE"; take_shot "$SHOT_FILE" "$OUT/fresh-video.log"
    stop_engine
}

peak_in() { awk -v a="$1" -v b="$2" '$1 >= a && $1 < b && $2 > m { m = $2 } END { print m + 0 }' "$OUT/audio-timeline.txt"; }

automute_check() {
    local id=3471294034 win_pid="" base muted back pmuted pback
    if [ ! -d "$LWE_DIR/wp-$id" ]; then echo "SKIP automute: wallpaper $id missing"; return; fi
    if ! command -v parec >/dev/null || ! command -v paplay >/dev/null || ! pactl info >/dev/null 2>&1; then
        echo "SKIP automute: no pulse tools or server in this guest"; return
    fi
    cat > "$OUT/peak.py" <<'PY'
import struct, subprocess, sys, time
p = subprocess.Popen(["parec", "-d", "@DEFAULT_SINK@.monitor", "--format=s16le", "--rate=8000", "--channels=1",
                      "--latency-msec=50"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
t0 = time.time()
while time.time() - t0 < float(sys.argv[1]):
    d = p.stdout.read(800)
    if not d:
        break
    s = struct.unpack("<%dh" % (len(d) // 2), d)
    print("%.1f %d" % (time.time() - t0, max(abs(x) for x in s)), flush=True)
p.kill()
PY
    python3 -c "
import wave
w = wave.open('$OUT/silence.wav', 'w'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b'\0' * 44100 * 4 * 12); w.close()"
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" $LWE_ARGS --automute "$LWE_DIR/wp-$id" > "$OUT/engine-automute.log" 2>&1 < /dev/null &)
    sleep "$SETTLE"
    python3 "$OUT/peak.py" 48 > "$OUT/audio-timeline.txt" 2>/dev/null &
    local mon=$!
    sleep 5
    paplay "$OUT/silence.wav" &
    sleep 17
    if [ -n "$FULLSCREEN_CMD" ]; then
        STOP_CMD=$FULLSCREEN_CMD; stop_fullscreen_window
        eval "$FULLSCREEN_CMD" > "$OUT/automute-window.log" 2>&1 &
        win_pid=$!
        sleep 8
        paplay "$OUT/silence.wav" &
        sleep 18
    else
        sleep 26
    fi
    wait "$mon" 2>/dev/null
    # a fullscreen window counts as sound for automute, the other app stopping can only be seen once the window is gone
    kill "$win_pid" 2>/dev/null; stop_fullscreen_window; sleep 4; stop_engine
    pkill -x paplay 2>/dev/null
    base=$(peak_in 1 5)
    if [ "$base" -lt 50 ]; then echo "SKIP automute: the wallpaper's music is not audible on this guest's default sink (peak $base)"; return; fi
    muted=$(peak_in 8 16); back=$(peak_in 18 22)
    if [ "$muted" -le 30 ]; then pass "automute: muted while another app plays (peak $muted, $base before)"; else fail "automute: not muted while another app plays (peak $muted)"; fi
    if [ "$back" -gt $((base / 4)) ]; then pass "automute: music back within 5s of the other app stopping (peak $back)"; else fail "automute: music not back 1.5-5s after the other app stopped (peak $back)"; fi
    if [ -n "$FULLSCREEN_CMD" ]; then
        # the scene's own music is silent while the wallpaper is paused, so what is checked is the engine's state: the log
        # lines of the second player come while the "Paused: windows" state holds
        local log=$OUT/engine-automute.log paused_line started stopped
        paused_line=$(grep -n "Paused: windows" "$log" | head -1 | cut -d: -f1)
        started=$(grep -n "Automute: another app is playing" "$log" | tail -1 | cut -d: -f1)
        stopped=$(grep -n "Automute: other apps stopped" "$log" | tail -1 | cut -d: -f1)
        if [ -z "$paused_line" ]; then echo "SKIP automute while paused: the engine was not paused by the window"
        elif [ "$(grep -c "Automute: another app is playing" "$log")" -ge 2 ] && [ "$started" -gt "$paused_line" ]; then pass "automute: another app's sound noticed while the wallpaper is paused"
        else fail "automute: another app's sound not noticed while the wallpaper is paused"; fi
        if [ -n "$paused_line" ] && [ -n "$stopped" ] && [ "$stopped" -gt "$started" ]; then pass "automute: the other app stopping noticed while the wallpaper is paused"
        else fail "automute: the other app stopping not noticed while the wallpaper is paused"; fi
    fi
}

hotswap_cycle_check() {
    local order="1120596121 1367456719 1078208425 1111948150 834237690 1367456719 1120596121"
    local ctl=$OUT/control-cycle log=$OUT/engine-cycle.log id pid n=0 swaps type helpers bad bad_before wait_s
    for id in $order; do
        if [ ! -d "$LWE_DIR/wp-$id" ]; then echo "SKIP hotswap cycle: wallpaper $id missing"; return; fi
    done
    : > "$ctl"
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" $LWE_ARGS --control-file "$ctl" "$LWE_DIR/wp-${order%% *}" > "$log" 2>&1 < /dev/null &)
    settle "$log" "${order%% *}"
    pid=$(engine_pids | head -1)
    if [ -z "$pid" ]; then fail "hotswap cycle: engine did not start"; return; fi
    for id in ${order#* }; do
        n=$((n + 1))
        if grep -qi '"type"[[:space:]]*:[[:space:]]*"web"' "$LWE_DIR/wp-$id/project.json"; then type=web; wait_s=20
        elif grep -qi '"type"[[:space:]]*:[[:space:]]*"video"' "$LWE_DIR/wp-$id/project.json"; then type=video; wait_s=12
        else type=scene; wait_s=12; fi
        swaps=$(grep -c "Hotswapping wallpaper to" "$log")
        bad_before=$(grep -ciE "terminate called|segmentation|fatal|exception" "$log")
        echo "path=$LWE_DIR/wp-$id" > "$ctl"
        kill -USR1 "$pid"
        for _ in $(seq 40); do [ "$(grep -c "Hotswapping wallpaper to" "$log")" -gt "$swaps" ] && break; sleep 0.5; done
        sleep "$wait_s"
        if ! kill -0 "$pid" 2>/dev/null; then fail "hotswap cycle step $n ($type $id): the engine exited"; return; fi
        if [ "$(grep -c "Hotswapping wallpaper to" "$log")" -le "$swaps" ]; then fail "hotswap cycle step $n ($type $id): no hotswap in the log"; stop_engine; return; fi
        helpers=$(pgrep -fc "[l]inux-wallpaperengine-web-helper")
        if [ "$type" = web ] && [ "$helpers" -eq 0 ]; then fail "hotswap cycle step $n ($type $id): no web host is running"; stop_engine; return; fi
        if [ "$type" != web ] && [ "$helpers" -gt 0 ]; then sleep 8; helpers=$(pgrep -fc "[l]inux-wallpaperengine-web-helper")
            if [ "$helpers" -gt 0 ]; then fail "hotswap cycle step $n ($type $id): $helpers web host processes left behind"; stop_engine; return; fi
        fi
        bad=$(grep -ciE "terminate called|segmentation|fatal|exception" "$log")
        if [ "$bad" -gt "$bad_before" ]; then fail "hotswap cycle step $n ($type $id): $((bad - bad_before)) new error lines in the log"; stop_engine; return; fi
        SHOT_FILE=$OUT/shot-cycle-$n-$id.png; rm -f "$SHOT_FILE"; take_shot "$SHOT_FILE" "$OUT/cycle-$n.log"
        pass "hotswap cycle step $n: $type $id"
    done
    stop_engine
}

multi_monitor_check() {
    local id=1111948150 w1 w2
    [ -n "$MULTI_SHOT_CMD" ] || return
    if ! command -v xrandr >/dev/null; then echo "SKIP multi monitor: no xrandr"; return; fi
    for f in /sys/class/drm/card*-Virtual-2/status; do echo on | sudo tee "$f" >/dev/null 2>&1; done
    sleep 2
    w1=$(xrandr | sed -n 's/^Virtual-1 connected[^0-9]*\([0-9]*\)x[0-9]*+.*/\1/p' | head -1)
    if [ -z "$w1" ] || ! xrandr | grep -q '^Virtual-2 connected'; then echo "SKIP multi monitor: the guest has no second output"; return; fi
    xrandr --output Virtual-1 --auto --pos 0x0 --output Virtual-2 --auto --pos "${w1}x0"
    sleep 4
    w2=$(xrandr | sed -n 's/^Virtual-2 connected[^0-9]*\([0-9]*\)x[0-9]*+.*/\1/p' | head -1)
    echo "$w1 $w2" > "$OUT/multi-geometry"
    SHOT_FILE=$OUT/multi-baseline.png; rm -f "$SHOT_FILE"; eval "$MULTI_SHOT_CMD" > "$OUT/multi-baseline.log" 2>&1
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" --screen-root Virtual-1 --screen-root Virtual-2 "$LWE_DIR/wp-$id" > "$OUT/engine-multi.log" 2>&1 < /dev/null &)
    settle "$OUT/engine-multi.log" "$id"
    if engine_pids >/dev/null; then pass "multi monitor: engine running on two screens"; else fail "multi monitor: engine exited"; fi
    SHOT_FILE=$OUT/multi-shot.png; rm -f "$SHOT_FILE"; eval "$MULTI_SHOT_CMD" > "$OUT/multi-shot.log" 2>&1
    if [ -s "$SHOT_FILE" ] && [ -s "$OUT/multi-baseline.png" ]; then pass "multi monitor: screenshots of both screens taken"; else fail "multi monitor: no screenshot ($(head -c 120 "$OUT/multi-shot.log" | tr '\n' ' '))"; fi
    stop_engine
    xrandr --output Virtual-2 --off
}

# behaviour checks, BEHAVIOR=1: hotswap, pause on fullscreen, SIGKILL leaves no helper
if [ "${BEHAVIOR:-0}" = 1 ] && want behavior; then
    A=${HOTSWAP_FROM:-1120596121}
    B=${HOTSWAP_TO:-1111948150}
    ctl=$OUT/control
    : > "$ctl"
    # shellcheck disable=SC2086
    (setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" $LWE_ARGS --control-file "$ctl" "$LWE_DIR/wp-$A" > "$OUT/engine-behavior.log" 2>&1 < /dev/null &)
    settle "$OUT/engine-behavior.log" "$A"
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
    settle "$OUT/engine-kill.log" 1078208425
    pid=$(engine_pids | head -1)
    helpers_before=$(pgrep -fc "[l]inux-wallpaperengine-web-helper")
    kill -9 "$pid" 2>/dev/null
    sleep 8
    helpers_after=$(pgrep -fc "[l]inux-wallpaperengine-web-helper")
    if [ "$helpers_before" -gt 0 ] && [ "$helpers_after" -eq 0 ]; then pass "SIGKILL: $helpers_before web host processes ended with the engine"
    elif [ "$helpers_before" -eq 0 ]; then fail "SIGKILL: no web host was running to begin with"
    else fail "SIGKILL: $helpers_after of $helpers_before web host processes left behind"; pkill -f "linux-wallpaperengine-web-helper"; fi
fi

if [ "${BEHAVIOR:-0}" = 1 ]; then
    want video && hotswap_video_check
    want cycle && hotswap_cycle_check
    want automute && automute_check
    want multi && multi_monitor_check
fi

touch "$OUT/done"
exit $status
