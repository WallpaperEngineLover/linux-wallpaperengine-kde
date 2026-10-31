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
# - The system bus is dead unless HEADLESS_RENDER_SYSTEM_BUS names one (e.g. dbusmock UPower), else the battery
#   check uses /sys/class/power_supply.
# - LWE_HEADLESS_CURSOR=x,y puts the GPU path's pointer at that fraction of the output (default 0.5,0.5).
# - HEADLESS_RENDER_WEB=1 makes web wallpapers work in a container: a private Xvfb (HEADLESS_RENDER_WEB_DISPLAY,
#   default :96), no Chromium sandbox (LWE_CEF_NO_SANDBOX=1) and libcef.so preloaded into the host
#   (LWE_WEB_HOST_PRELOAD, else Chromium's close() wrapper crashes). WebGL runs on the GPU via headless ozone + ANGLE
#   EGL (LWE_CEF_SWITCHES overrides). Use HEADLESS_RENDER_DELAY=600 or more.
# - --screenshot-delay is capped at 5000 frames by the engine (ApplicationContext.cpp).
# - CImage.cpp's puppet/effect diagnostics are one-shot logs that fire on the first draw call,
#   not a chosen frame.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <binary> <output.png> [engine args...]" >&2
    exit 1
fi

BINARY="$1"; shift
# the sandbox shares the desktop's user, a crashing test render would land in its crash handler (DrKonqi)
ulimit -c 0
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
# a private runtime dir and no desktop variables: libwayland falls back to $XDG_RUNTIME_DIR/wayland-0 when
# WAYLAND_DISPLAY is unset, so a build without the headless driver would otherwise open a window on the real
# desktop session (and reach its D-Bus / audio server)
RUNTIME_DIR="$(mktemp -d /tmp/lwe-headless-run.XXXXXX)"
chmod 700 "$RUNTIME_DIR"
DBUS_PID=
WEB_XVFB_PID=
XVFB_PID=
HELPER_PID=
trap 'kill $DBUS_PID $HELPER_PID $WEB_XVFB_PID $XVFB_PID 2>/dev/null || true; rm -rf "$RUNTIME_DIR"' EXIT
BUS_ADDRESS=unix:path=/nonexistent-bus
if command -v dbus-daemon >/dev/null; then
    DBUS_PID=$(dbus-daemon --session --fork --print-pid --address="unix:path=$RUNTIME_DIR/bus")
    BUS_ADDRESS="unix:path=$RUNTIME_DIR/bus"
fi
# HEADLESS_RENDER_BUS_HELPER: a shell command started on the private session bus (DBUS_SESSION_BUS_ADDRESS set), e.g. a
# fake MPRIS player (tools/desktops/fake_mpris_player.py), stopped with the render
if [ -n "${HEADLESS_RENDER_BUS_HELPER:-}" ] && [ -n "$DBUS_PID" ]; then
    DBUS_SESSION_BUS_ADDRESS="$BUS_ADDRESS" bash -c "$HEADLESS_RENDER_BUS_HELPER" > /dev/null 2>&1 &
    HELPER_PID=$!
fi
# HEADLESS_RENDER_PULSE_SERVER: private audio server to play into (tools/wecompare/wine/audio_sink.sh), never the desktop's
PULSE=unix:/nonexistent-pulse
if [ -n "${HEADLESS_RENDER_PULSE_SERVER:-}" ]; then
    PULSE_SOCKET="${HEADLESS_RENDER_PULSE_SERVER#unix:}"
    if [[ "$PULSE_SOCKET" != /* || "$PULSE_SOCKET" == *[[:space:]]* ]]; then
        echo "HEADLESS_RENDER_PULSE_SERVER must be a unix socket path, got $HEADLESS_RENDER_PULSE_SERVER" >&2
        exit 1
    fi
    DESKTOP_PULSE=("${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native" "/run/user/$(id -u)/pulse/native")
    if [[ "${PULSE_SERVER:-}" == unix:/* ]]; then DESKTOP_PULSE+=("${PULSE_SERVER#unix:}"); fi
    if [[ "${PULSE_SERVER:-}" == /* ]]; then DESKTOP_PULSE+=("$PULSE_SERVER"); fi
    if [[ "$(realpath -m "$PULSE_SOCKET")" == /run/user/* ]]; then
        echo "HEADLESS_RENDER_PULSE_SERVER must not be the desktop's audio server" >&2
        exit 1
    fi
    if [ -S "$PULSE_SOCKET" ]; then
        for candidate in "${DESKTOP_PULSE[@]}"; do
            if [ -S "$candidate" ] && [ "$(stat -Lc %d:%i "$candidate")" = "$(stat -Lc %d:%i "$PULSE_SOCKET")" ]; then
                echo "HEADLESS_RENDER_PULSE_SERVER must not be the desktop's audio server" >&2
                exit 1
            fi
        done
    fi
    PULSE=unix:$PULSE_SOCKET
fi
ISOLATE=(-u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u KDE_FULL_SESSION -u KDE_SESSION_VERSION
    -u DESKTOP_SESSION -u XDG_CURRENT_DESKTOP -u XDG_SESSION_DESKTOP XDG_RUNTIME_DIR="$RUNTIME_DIR"
    DBUS_SESSION_BUS_ADDRESS="$BUS_ADDRESS"
    DBUS_SYSTEM_BUS_ADDRESS="${HEADLESS_RENDER_SYSTEM_BUS:-unix:path=/nonexistent-system-bus}"
    PULSE_SERVER="$PULSE" PIPEWIRE_REMOTE=/nonexistent-pipewire)
if [ -n "$USE_GPU" ]; then
    SESSION=(env -u DISPLAY "${ISOLATE[@]}" XDG_SESSION_TYPE=headless)
else
    SESSION=(env "${ISOLATE[@]}" DISPLAY="$XVFB_DISPLAY" XDG_SESSION_TYPE=x11)
fi

if [ -n "${HEADLESS_RENDER_WEB:-}" ]; then
    WEB_DISPLAY="${HEADLESS_RENDER_WEB_DISPLAY:-:96}"
    if ! xdpyinfo -display "$WEB_DISPLAY" >/dev/null 2>&1; then
        Xvfb "$WEB_DISPLAY" -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 &
        WEB_XVFB_PID=$!
        for _ in $(seq 1 20); do
            xdpyinfo -display "$WEB_DISPLAY" >/dev/null 2>&1 && break
            sleep 0.2
        done
    fi
    # headless ozone + ANGLE EGL for GPU WebGL, shared memory in /tmp: Docker's 64MB /dev/shm loses the context on big
    # uploads (3496522892)
    SESSION+=(DISPLAY="$WEB_DISPLAY" LWE_CEF_NO_SANDBOX=1
        LWE_WEB_HOST_PRELOAD="${LWE_WEB_HOST_PRELOAD:-$(dirname "$(readlink -f "$BINARY")")/libcef.so}"
        LWE_CEF_SWITCHES="${LWE_CEF_SWITCHES:---ozone-platform=headless --use-gl=angle --use-angle=gl-egl --ignore-gpu-blocklist --disable-dev-shm-usage}")
fi

if [ -z "$USE_GPU" ] && [ ! -S "$XVFB_SOCKET" ]; then
    Xvfb "$XVFB_DISPLAY" -screen 0 1920x1080x24 &
    XVFB_PID=$!
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
