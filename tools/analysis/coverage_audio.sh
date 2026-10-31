#!/usr/bin/env bash
# The audio and LED paths of a coverage build: sound scenes against a private PulseAudio server (never the desktop's;
# tools/wecompare/wine/audio_sink.sh), first with a plain null sink, then with a default sink that calls itself a headset
# so the headphone detector and HRTF switch on, a web wallpaper with audio for the per process volume, and the LED
# output against the fake OpenRGB server of tools/check_leds.sh.
# Usage: tools/analysis/coverage_audio.sh <engine binary> [assets dir] [workshop content dir]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY=${1:?engine binary}
ASSETS=${2:-/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets}
CONTENT=${3:-/workspace/SteamLibrary/steamapps/workshop/content/431960}
SINK="$HERE/wecompare/wine/audio_sink.sh"
work=$(mktemp -d /tmp/lwe-coverage-audio.XXXXXX)
trap '"$SINK" stop > /dev/null 2>&1; rm -rf "$work"' EXIT

"$SINK" start || { echo "SKIP: no private PulseAudio"; exit 0; }
eval "$("$SINK" env)"
export HEADLESS_RENDER_PULSE_SERVER=$PULSE_SERVER
private_pactl () { PULSE_SERVER=$HEADLESS_RENDER_PULSE_SERVER pactl "$@"; }
# headless_render.sh refuses a PULSE_SERVER in its own environment, it is handed the private one explicitly
unset PULSE_SERVER

scenes=${SOUND_SCENES:-"3509578940 3737268876 3354366708 3287715210"}
render () {
    local label=$1
    shift
    if HEADLESS_RENDER_DELAY=250 HEADLESS_RENDER_TIMEOUT=120 HEADLESS_RENDER_SIZE=800x450 \
        "$HERE/headless_render.sh" "$BINARY" "$work/shot.png" --assets-dir "$ASSETS" "$@" > "$work/run.log" 2>&1; then
        echo "ok   $label"
    else
        echo "FAIL $label"
        tail -3 "$work/run.log"
    fi
    rm -f "$work/shot.png"
}

for id in $scenes; do
    [ -d "$CONTENT/$id" ] && render "scene $id on a speaker sink" --volume 64 "$CONTENT/$id"
done

private_pactl load-module module-null-sink sink_name=lwe_headset rate=48000 channels=2 \
    sink_properties=device.form_factor=headset > /dev/null && private_pactl set-default-sink lwe_headset
for id in $scenes; do
    [ -d "$CONTENT/$id" ] && render "scene $id on a headset sink" --volume 64 --hrtf auto "$CONTENT/$id"
done
for id in $scenes; do
    [ -d "$CONTENT/$id" ] && render "scene $id with hrtf off" --volume 64 --hrtf off "$CONTENT/$id" && break
done

for id in 1078208425 3026605859; do
    [ -d "$CONTENT/$id" ] && HEADLESS_RENDER_WEB=1 render "web $id with sound" --volume 40 --ambient-volume 30 "$CONTENT/$id"
done

"$HERE/check_leds.sh" "$BINARY" "$CONTENT/3509578940" "$work/leds" --assets-dir "$ASSETS" > "$work/leds.log" 2>&1 \
    && echo "ok   leds" || { echo "FAIL leds"; tail -3 "$work/leds.log"; }
