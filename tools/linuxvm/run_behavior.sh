#!/bin/bash
# run_behavior.sh <vm> <session screen name> <fullscreen command> - the behaviour phase of guest_test.sh on a running VM
# (hotswap, pause on fullscreen, SIGKILL cleanup) with host side screenshots, then checks the hotswap pictures differ.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 screen=$2 fullscreen=$3
out=${OUT_DIR:-/tmp/lwe-test}
"$HERE/linuxvm.sh" scp "$vm" "$HERE/guest_test.sh" lwe/guest_test.sh
"$HERE/linuxvm.sh" scp "$vm" "$HERE/fullscreen_window.py" lwe/fullscreen_window.py
"$HERE/linuxvm.sh" ssh "$vm" "rm -rf $out; mkdir -p $out" </dev/null
"$HERE/host_shots.sh" "$vm" "$out" 500 > /dev/null 2>&1 &
"$HERE/linuxvm.sh" ssh "$vm" ". ~/sessenv; BEHAVIOR=1 WALLPAPERS= SETTLE=${SETTLE:-25} LWE_ARGS='--screen-root $screen' SHOT_CMD=host FULLSCREEN_CMD='$fullscreen' ~/lwe/guest_test.sh" </dev/null
wait
local=$(mktemp -d)
scp -q -r -i "$HOME/.local/share/linuxvm/id_ed25519" -P "$(cat "$HOME/.local/share/linuxvm/vms/$vm/port")" -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "lab@127.0.0.1:$out/*.png" "$local/" 2>/dev/null
python3 -I "$HERE/check_frame.py" --differ "$local/hot-before.png" "$local/hot-after.png"
echo "pictures in $local"
