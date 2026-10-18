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
WALLPAPERS=${WALLPAPERS:-"1111948150 1120596121 1078208425 834237690"}
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
        for _ in $(seq 60); do [ -s "$1" ] && break; sleep 1; done
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
touch "$OUT/done"
exit $status
