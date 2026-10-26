#!/bin/bash
# builds the stub with zig (python3 -m ziglang) and puts it next to a live WE install: build.sh [we28 dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WE=${1:-${WE_LIVE_DIR:-$HOME/.local/share/we_live}/we28}
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -shared -o "$WE/CUESDK.x64_2017.dll" "$HERE/cuesdk_stub.c"
echo "stub installed in $WE (remove the file to get the plugin without it)"
