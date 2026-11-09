#!/usr/bin/env bash
# Checks --audio-input, --audio-input-volume and --audio-input-threshold against a private PulseAudio server (never the
# desktop's, tools/wecompare/wine/audio_sink.sh): a tone plays into a null sink that stands for a microphone while the
# default output stays silent, and the engine's own capture statistics (LWE_AUDIO_STATS) say what it listened to.
# Usage: tools/check_audio_input.sh <engine binary> [scene dir] [assets dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=${1:?engine binary}
SCENE=${2:-/workspace/SteamLibrary/steamapps/workshop/content/431960/1509527803}
ASSETS=${3:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
SINK="$HERE/wecompare/wine/audio_sink.sh"
work=$(mktemp -d /tmp/lwe-audio-input.XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; "$SINK" stop > /dev/null 2>&1; rm -rf "$work"' EXIT

[ -d "$SCENE" ] || { echo "SKIP: $SCENE is not installed"; exit 0; }
"$SINK" start || { echo "SKIP: no private PulseAudio"; exit 0; }
eval "$("$SINK" env)"
export HEADLESS_RENDER_PULSE_SERVER=$PULSE_SERVER
unset PULSE_SERVER
private_pactl () { PULSE_SERVER=$HEADLESS_RENDER_PULSE_SERVER pactl "$@"; }

private_pactl load-module module-null-sink sink_name=lwe_mic rate=44100 channels=2 sink_properties=device.description=lwe_mic > /dev/null || exit 1

failures=0
fail () { echo "FAIL $1"; failures=$((failures + 1)); }

# a 440 Hz tone (sine's 1/8 amplitude times the given factor) into the "microphone" sink for 14 seconds
play_tone () {
    PULSE_SERVER=$HEADLESS_RENDER_PULSE_SERVER ffmpeg -loglevel quiet -f lavfi -i "sine=frequency=440:sample_rate=44100" -af "volume=$1,aformat=channel_layouts=stereo" \
        -t 14 -f pulse -device lwe_mic tone > /dev/null 2>&1 &
}

# runs the engine for a few seconds and keeps its capture statistics lines in $work/<label>.stats
run_engine () {
    local label=$1
    shift
    : > "$work/run.log"
    LWE_AUDIO_STATS=1 HEADLESS_RENDER_DELAY=4000 HEADLESS_RENDER_TIMEOUT=9 HEADLESS_RENDER_SIZE=640x360 \
        HEADLESS_RENDER_KEEP_RUNNING=1 "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" \
        "$@" "$SCENE" > "$work/run.log" 2>&1
    grep "Audio capture:" "$work/run.log" > "$work/$label.stats"
}

field () {
    # the largest value of the last three statistics lines, $1 label, $2 field name
    local pattern="$2 \\([0-9.e+-]*\\)"
    [ "$2" = blocks ] && pattern="\\([0-9]*\\) blocks"
    grep "Audio capture:" "$work/$1.stats" | tail -3 | sed -n "s/.*[ ,]$pattern.*/\\1/p" | sort -g | tail -1
}

play_tone 3
sleep 1
run_engine default
peak=$(field default peak)
[ -n "$peak" ] && awk "BEGIN { exit !($peak == 0) }" && echo "ok   the default output stays silent (peak $peak)" \
    || fail "default capture heard the microphone sink (peak ${peak:-none})"

wait
play_tone 3
sleep 1
run_engine mic --audio-input lwe_mic.monitor
peak=$(field mic peak)
blocks=$(field mic blocks)
awk "BEGIN { exit !(${peak:-0} > 0.2) }" && echo "ok   --audio-input hears the tone (peak $peak, $blocks blocks)" \
    || fail "--audio-input lwe_mic.monitor heard nothing (peak ${peak:-none})"
low50=$(field mic low)

wait
play_tone 3
sleep 1
run_engine named --audio-input "Monitor of lwe_mic"
peak=$(field named peak)
awk "BEGIN { exit !(${peak:-0} > 0.2) }" && echo "ok   a source is found by its description (peak $peak)" \
    || fail "--audio-input by description heard nothing (peak ${peak:-none})"

wait
play_tone 3
sleep 1
run_engine sinkname --audio-input lwe_mic
peak=$(field sinkname peak)
awk "BEGIN { exit !(${peak:-0} > 0.2) }" && echo "ok   a sink name selects its monitor (peak $peak)" \
    || fail "--audio-input lwe_mic (a sink) heard nothing (peak ${peak:-none})"

wait
play_tone 3
sleep 1
run_engine loud --audio-input lwe_mic.monitor --audio-input-volume 100
low100=$(field loud low)
if awk "BEGIN { exit !(${low50:-0} > 0 && ${low100:-0} / ${low50:-1} > 1.8 && ${low100:-0} / ${low50:-1} < 2.2) }"; then
    echo "ok   --audio-input-volume 100 doubles the bands ($low50 -> $low100)"
else
    fail "volume 100 should double the bands of volume 50 ($low50 -> ${low100:-none})"
fi

wait
play_tone 3
sleep 1
run_engine gated --audio-input lwe_mic.monitor --audio-input-threshold 900
blocks=$(field gated blocks)
[ "${blocks:-1}" = "0" ] && echo "ok   a threshold above the tone's level silences it" \
    || fail "threshold 900 should gate the tone (blocks ${blocks:-none})"

wait
play_tone 3
sleep 1
run_engine open --audio-input lwe_mic.monitor --audio-input-threshold 100
blocks=$(field open blocks)
awk "BEGIN { exit !(${blocks:-0} > 0) }" && echo "ok   a threshold below the tone's level lets it through" \
    || fail "threshold 100 should pass the tone (blocks ${blocks:-none})"

run_engine missing --audio-input no_such_source
grep -q "Audio input 'no_such_source' not found" "$work/run.log" && echo "ok   an unknown input falls back with a message" \
    || fail "unknown input was not reported"

exit $((failures > 0))
