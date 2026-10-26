#!/bin/bash
# usage: we_led.sh <scene dir or scene.json/.pkg> <out dir>
# Real WE 2.8.42 under Wine with its LED plugin (plugins/led/ledextensions64.dll) talking to the CUE stub in cuestub/, so the
# 80 x 10 frame WE hands the plugin lands in $WE_CUE_DIR/probe_<n>.ppm (default /tmp/lwe-cuestub, raw: boost is switched off).
# WE_LED_WAIT seconds (default 30) go by before the screenshot in <out dir>.
#
# Needs, once (nothing of the Steam install is overwritten, we28/wallpaper64.exe is the rig's own copy):
#   - cuestub/build.sh, and the CUESDK.x64_2017.dll it installs into we28/
#   - python3 we_patch_led.py $S/we28/wallpaper64.exe: the exe loads plugins on startup only when started by the UI, the patch makes it always
#   - gdb (Fedora: dnf install gdb)
# The stub logs every call to calls.log, see the header of cuestub/cuesdk_stub.c
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
CUE=${WE_CUE_DIR:-/tmp/lwe-cuestub}
rm -rf "$CUE"
WE_LED=1 WE_WAIT=${WE_LED_WAIT:-30} bash "$HERE/we_full.sh" "$1" "$2" 1 1 > /dev/null 2>&1 &
sleep $((${WE_LED_ATTACH:-30}))
pid=$(pgrep -x wallpaper64.exe | head -1)
ulimit -c 0
LED_RAW=1 timeout 30 gdb -q -batch -p "$pid" -x "$HERE/gdb_force_led.py" 2>&1 | grep '^forced'
wait
