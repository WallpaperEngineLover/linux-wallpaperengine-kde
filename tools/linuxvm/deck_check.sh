#!/bin/bash
# deck_check.sh - in the steamdeck VM: the Steam Deck preset's engine arguments run, and the engine follows the fake
# battery (the test_power kernel module, seen by UPower like a real one) down to battery FPS and back to external power.
# Env: SCREEN (the --screen-root name). Prints PASS/FAIL lines.
LWE_DIR=$HOME/lwe
ENGINE=$LWE_DIR/lwe-hints/linux-wallpaperengine
WP=$LWE_DIR/wp-3471294034
OUT=/tmp/lwe-test/deck
PARAMS=/sys/module/test_power/parameters
mkdir -p "$OUT"

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; }

power() {
    sudo -n sh -c "echo $1 > $PARAMS/ac_online; echo $2 > $PARAMS/battery_status"
}

wait_for() {
    local pattern=$1 count=$2 n
    for n in $(seq 30); do
        [ "$(grep -c "$pattern" "$OUT/engine.log")" -ge "$count" ] && return 0
        sleep 1
    done
    return 1
}

[ -d $PARAMS ] || sudo -n modprobe test_power
power off discharging
for _ in $(seq 10); do
    [ "$(busctl --system get-property org.freedesktop.UPower /org/freedesktop/UPower org.freedesktop.UPower OnBattery)" = "b true" ] && break
    sleep 1
done
if [ "$(busctl --system get-property org.freedesktop.UPower /org/freedesktop/UPower org.freedesktop.UPower OnBattery)" = "b true" ]; then
    pass "deck: UPower reports the fake battery as discharging"
else
    fail "deck: UPower does not report the fake battery as discharging"
    exit 0
fi

# what we_manager's Steam Deck preset launches with
args="--fps 30 --no-audio-processing --volumetrics low --shadows low --fps-battery 15 --idle-pause 5"
(setsid nohup "$ENGINE" --assets-dir "$LWE_DIR/assets" --screen-root "$SCREEN" $args "$WP" > "$OUT/engine.log" 2>&1 < /dev/null &)

if wait_for "Power: running on battery" 1; then
    pass "deck: preset arguments start the engine, it sees the battery"
else
    fail "deck: no 'running on battery' line with the preset arguments ($(tail -n 3 "$OUT/engine.log" | tr '\n' ' '))"
fi

power on charging
if wait_for "Power: on external power" 1; then
    pass "deck: engine notices the charger"
else
    fail "deck: engine did not notice the charger"
fi

power off discharging
if wait_for "Power: running on battery" 2; then
    pass "deck: engine notices the charger being unplugged"
else
    fail "deck: engine did not notice the battery again"
fi

if pgrep -f "[l]inux-wallpaperengine.bin.*fps-battery 15" > /dev/null; then
    pass "deck: engine still running"
else
    fail "deck: engine died"
fi
pkill -f "[l]inux-wallpaperengine.bin.*fps-battery 15"
exit 0
