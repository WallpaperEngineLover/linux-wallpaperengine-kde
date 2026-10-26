#!/usr/bin/env bash
# Renders a scene headlessly with --leds against tools/openrgb_probe.py (a fake OpenRGB server with one 80 x 10 device)
# and prints the last frame the device got, as a PPM and an upscaled PNG.
#
# Usage: tools/check_leds.sh <binary> <scene dir or pkg> <out dir> [engine args...]
# The scene's frame is compared by eye or with tools/wecompare against the plugin's frame under Wine
# (tools/wecompare/wine/we_led.sh). --leds-no-boost is passed so the raw frame comes out
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <binary> <scene> <out dir> [engine args...]" >&2
    exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
BINARY=$1
SCENE=$2
OUT=$3
shift 3
PORT=${LEDS_PROBE_PORT:-16742}
ASSETS=${ASSETS_DIR:-}

rm -rf "$OUT"
mkdir -p "$OUT"
python3 "$HERE/openrgb_probe.py" --port "$PORT" --out "$OUT/frames" > "$OUT/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true' EXIT
sleep 1

extra=()
[ -n "$ASSETS" ] && extra+=(--assets-dir "$ASSETS")
HEADLESS_RENDER_KEEP_RUNNING=1 HEADLESS_RENDER_TIMEOUT=${LEDS_RUN_SECONDS:-12} "$HERE/headless_render.sh" "$BINARY" "$OUT/screenshot.png" \
    "${extra[@]}" --fps 30 --leds-server "127.0.0.1:$PORT" --leds-no-boost "$@" "$SCENE" > "$OUT/engine.log" 2>&1 || true

last=$(ls "$OUT"/frames/probe_*.ppm 2>/dev/null | tail -1)
[ -n "$last" ] || { echo "no frame reached the probe server, see $OUT/engine.log" >&2; exit 1; }
cp "$last" "$OUT/frame.ppm"
python3 - "$OUT/frame.ppm" "$OUT/frame.png" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).resize((800, 100), Image.NEAREST).save(sys.argv[2])
PY
echo "$OUT/frame.ppm"
