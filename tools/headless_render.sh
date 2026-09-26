#!/usr/bin/env bash
# Renders a wallpaper headlessly and takes a screenshot, for environments with no way to see the
# real screen (sandboxes, CI, this project's own dev container). When a GPU render node
# (/dev/dri/renderD*) is accessible it renders there through the engine's headless EGL driver
# (XDG_SESSION_TYPE=headless, no X server, a 4K scene takes ~1s instead of ~20s). Otherwise, or
# with HEADLESS_RENDER_SOFTWARE=1, it falls back to Xvfb + Mesa llvmpipe software rendering.
# LWE_HEADLESS_DEVICE=/dev/dri/renderDN picks the GPU when there are several.
#
# Usage:
#   tools/headless_render.sh <path-to-linux-wallpaperengine-binary> <output.png> [engine args...]
#
# Example:
#   tools/headless_render.sh build/output/linux-wallpaperengine /tmp/out.png \
#       --assets-dir /path/to/wallpaper_engine/assets --window 0x0x1920x1080 --fps 25 \
#       /path/to/workshop/item/dir
#
# Requirements: a GPU render node, or Xvfb and a software GL renderer (mesa llvmpipe is normally
# already present alongside libgl1-mesa-dri); gcc (to build the D-Bus shim once, cached after that).
#
# Known limitations:
# - No real D-Bus session bus is assumed - dbus_noop_shim.so (built automatically) patches the
#   one call path that isn't already null-safe against a missing bus.
# - The engine is stopped as soon as the screenshot file appears (HEADLESS_RENDER_KEEP_RUNNING=1 disables
#   that and runs until HEADLESS_RENDER_TIMEOUT, default 60s).
# - HEADLESS_RENDER_SIZE=WxH changes the window size (default 1920x1080).
# - --screenshot-delay is capped at 5000 frames by the engine (ApplicationContext.cpp).
# - CImage.cpp's puppet/effect diagnostics are one-shot logs that fire on the first draw call,
#   not a chosen frame.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <binary> <output.png> [engine args...]" >&2
    exit 1
fi

BINARY="$1"; shift
OUTPUT="$1"; shift

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY_DIR="$(cd "$(dirname "$BINARY")" && pwd)"
SHIM_SRC="$SCRIPT_DIR/dbus_noop_shim.c"
SHIM_SO="/tmp/dbus_noop_shim.so"

if [ ! -f "$SHIM_SO" ] || [ "$SHIM_SRC" -nt "$SHIM_SO" ]; then
    gcc -shared -fPIC -o "$SHIM_SO" "$SHIM_SRC" -ldl
fi

USE_GPU=
if [ -z "${HEADLESS_RENDER_SOFTWARE:-}" ]; then
    for node in /dev/dri/renderD*; do
        if [ -r "$node" ] && [ -w "$node" ]; then
            USE_GPU=1
            break
        fi
    done
fi

XVFB_DISPLAY="${HEADLESS_RENDER_DISPLAY:-:99}"
XVFB_SOCKET="/tmp/.X11-unix/X${XVFB_DISPLAY#:}"
if [ -n "$USE_GPU" ]; then
    SESSION=(env -u DISPLAY -u WAYLAND_DISPLAY XDG_SESSION_TYPE=headless)
else
    SESSION=(env -u WAYLAND_DISPLAY DISPLAY="$XVFB_DISPLAY" XDG_SESSION_TYPE=x11)
fi

if [ -z "$USE_GPU" ] && [ ! -S "$XVFB_SOCKET" ]; then
    Xvfb "$XVFB_DISPLAY" -screen 0 1920x1080x24 &
    XVFB_PID=$!
    trap 'kill "$XVFB_PID" 2>/dev/null || true' EXIT
    # give it a moment to bind before launching anything against it
    for _ in $(seq 1 20); do
        [ -S "$XVFB_SOCKET" ] && break
        sleep 0.2
    done
fi

rm -f "$OUTPUT"

"${SESSION[@]}" \
LD_LIBRARY_PATH="$BINARY_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
LD_PRELOAD="$SHIM_SO" \
timeout "${HEADLESS_RENDER_TIMEOUT:-60}" "$BINARY" \
    --window "0x0x${HEADLESS_RENDER_SIZE:-1920x1080}" \
    --screenshot "$OUTPUT" \
    --screenshot-delay "${HEADLESS_RENDER_DELAY:-3}" \
    "$@" &
ENGINE_PID=$!

# the engine keeps rendering after the screenshot is taken, so without this the run always lasts
# the full timeout - stop it as soon as the file is written (set HEADLESS_RENDER_KEEP_RUNNING=1 to
# let it run until the timeout instead)
if [ -z "${HEADLESS_RENDER_KEEP_RUNNING:-}" ]; then
    while kill -0 "$ENGINE_PID" 2>/dev/null; do
        if [ -s "$OUTPUT" ]; then
            # wait for the file to stop growing so a half-written PNG isn't cut off
            prev=-1
            cur=$(stat -c %s "$OUTPUT")
            while [ "$cur" != "$prev" ]; do
                sleep 0.3
                prev=$cur
                cur=$(stat -c %s "$OUTPUT")
            done
            kill "$ENGINE_PID" 2>/dev/null || true
            break
        fi
        sleep 0.2
    done
fi
wait "$ENGINE_PID" || true

if [ -f "$OUTPUT" ]; then
    echo "Wrote $OUTPUT"
else
    echo "No screenshot was produced - check the engine's stderr output above" >&2
    exit 1
fi
