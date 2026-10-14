#!/bin/bash
# headphones_reconnect.sh [--pulse-server]: HeadphonesDetector across a pipewire-pulse restart, on a private D-Bus +
# PipeWire + pipewire-pulse in their own runtime dir (nothing reaches /run/user or the system bus). Prints the probe's
# answers and every connect() to the pulse socket: after the kill the detector should wait for the server's
# org.pulseaudio.Server name on D-Bus (no connects until the restart), with --pulse-server (PULSE_SERVER set, libpulse
# can't wait on D-Bus then) it retries every 2 s. Needs strace and a built engine library
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
R=${TMPDIR:-/tmp}/lwe-hp-$$
g++ -std=c++20 -O1 "$HERE/headphones_probe.cpp" -I"$REPO/src" -I"$REPO/build/generated" $(pkg-config --cflags libpulse) \
    -L"$REPO/build/output" -llinux-wallpaperengine-lib -Wl,-rpath,"$REPO/build/output" $(pkg-config --libs libpulse) \
    -o "$R.probe" || exit 1
PS=(); [ "$1" = "--pulse-server" ] && PS=(PULSE_SERVER=unix:$R/native)
rm -rf $R; mkdir -p $R/config $R/state $R/data; chmod 700 $R
ADDR=$(dbus-daemon --session --fork --print-address=1 --print-pid=3 3>$R/dbus.pid)
E=(env -i HOME=$R PATH=/usr/bin:/usr/sbin XDG_RUNTIME_DIR=$R PULSE_RUNTIME_PATH=$R PIPEWIRE_RUNTIME_DIR=$R
   XDG_CONFIG_HOME=$R/config XDG_STATE_HOME=$R/state XDG_DATA_HOME=$R/data DBUS_SESSION_BUS_ADDRESS=$ADDR
   DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent ALSA_CONFIG_PATH=/dev/null)
start_pulse () {
    "${E[@]}" setsid pipewire-pulse > $R/pipewire-pulse.log 2>&1 &
    echo $! > $R/pp.pid
    for i in $(seq 50); do "${E[@]}" pactl info >/dev/null 2>&1 && break; sleep 0.1; done
    "${E[@]}" pactl load-module module-null-sink sink_name=hp sink_properties=device.form_factor=headphone >/dev/null
    "${E[@]}" pactl set-default-sink hp
}
"${E[@]}" setsid pipewire > $R/pipewire.log 2>&1 &
sleep 0.5
"${E[@]}" setsid wireplumber > $R/wireplumber.log 2>&1 &
sleep 0.5
start_pulse
echo "server up: $("${E[@]}" pactl get-default-sink)"
"${E[@]}" "${PS[@]}" strace -f -tt -e trace=connect -o $R/strace.log "$R.probe" 16 &
H=$!
sleep 3
echo "-- killing pipewire-pulse at $(date +%T.%N | cut -c1-12)"
kill $(cat $R/pp.pid)
sleep 6
echo "-- restarting pipewire-pulse at $(date +%T.%N | cut -c1-12)"
start_pulse
wait $H
for p in $(pgrep -x pipewire) $(pgrep -x wireplumber) $(pgrep -x pipewire-pulse); do
    { tr '\0' '\n' < /proc/$p/environ; } 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$R" && kill $p
done
kill $(cat $R/dbus.pid)
echo "-- connects to the pulse socket:"
grep -c "native" $R/strace.log
grep "native" $R/strace.log | cut -c1-160
rm -rf "$R" "$R.probe"
