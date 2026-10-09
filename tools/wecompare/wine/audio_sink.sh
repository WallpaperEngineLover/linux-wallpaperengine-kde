#!/bin/bash
# usage: audio_sink.sh start|stop|env|record <out.wav> <seconds>
# private PulseAudio with a single null sink, no connection to the desktop's audio server.
# `eval "$(audio_sink.sh env)"` points a process at it, `record` captures the sink's monitor (float32 stereo 48 kHz
# WAV). Clients need PULSE_LATENCY_MSEC (set by env), otherwise the monitor delivers in 2 s blocks; parec takes
# ~0.5 s to connect, so align recordings by onset
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
R=$S/run/pulse-null
SOCK=$R/native

env_lines() {
    echo "export PULSE_SERVER=unix:$SOCK PULSE_COOKIE=$R/cookie PULSE_LATENCY_MSEC=20 PIPEWIRE_REMOTE=$S/run/no-pipewire ALSA_CONFIG_PATH=/dev/null"
}

running() { [ -S "$SOCK" ] && PULSE_SERVER=unix:$SOCK PULSE_COOKIE=$R/cookie pactl info >/dev/null 2>&1; }

case "$1" in
start)
    running && exit 0
    mkdir -p "$R"
    cat > "$R/default.pa" <<EOF
load-module module-native-protocol-unix socket=$SOCK auth-anonymous=1
load-module module-null-sink sink_name=lwe_null rate=48000 channels=2 format=float32le sink_properties=device.description=lwe_null
set-default-sink lwe_null
set-default-source lwe_null.monitor
EOF
    # own runtime/state dirs and no autospawn, so it never finds the host's server
    env -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR=$R PULSE_RUNTIME_PATH=$R PULSE_STATE_PATH=$R \
        PULSE_CONFIG_PATH=$R PULSE_COOKIE=$R/cookie \
        nohup pulseaudio -n --daemonize=no --exit-idle-time=-1 --use-pid-file=no --disallow-module-loading \
        --log-target=file:$R/log -F "$R/default.pa" > /dev/null 2>&1 &
    for i in $(seq 50); do running && exit 0; sleep 0.1; done
    echo "pulseaudio did not come up, see $R/log" >&2; exit 1 ;;
stop)
    pkill -f "pulseaudio -n .*$R/default.pa" ;;
env)
    env_lines ;;
record)
    running || { echo "not running" >&2; exit 1; }
    eval "$(env_lines)"
    # a silent low latency stream keeps the sink running so the monitor delivers a continuous timeline
    pacat -p -d lwe_null --latency-msec=20 --rate=48000 --channels=2 --format=float32le < /dev/zero & KEEP=$!
    sleep 0.3
    timeout -s INT "$3" parec -d lwe_null.monitor --latency-msec=20 --rate=48000 --channels=2 --format=float32le --file-format=wav "$2"
    kill $KEEP 2>/dev/null
    [ -s "$2" ] ;;
*)
    sed -n 2,5p "$0"; exit 1 ;;
esac
