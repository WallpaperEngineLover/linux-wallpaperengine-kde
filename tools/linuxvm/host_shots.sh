#!/bin/bash
# host_shots.sh <vm> <guest out dir> [seconds] - serves the "SHOT_CMD=host" requests of guest_test.sh: whenever the guest
# creates <file>.png.want, takes a screenshot of the VM's display (the same VNC socket `linuxvm.sh shot` uses) and copies
# it into the guest as <file>.png, and clicks the screen's centre for every <file>.click (focus for a test window). Runs until the seconds (default 900) are over or the guest's test run has finished
# (the guest creates <out dir>/done). A pointer move comes first, a guest whose screen went idle (Steam Deck VM) shows "Display output is not active" otherwise.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 out=$2 limit=${3:-900}
tmp=$(mktemp -d)
end=$((SECONDS + limit))
while [ $SECONDS -lt $end ]; do
    wanted=$("$HERE/linuxvm.sh" ssh "$vm" "find $out -maxdepth 1 \\( -name '*.want' -o -name '*.click' \\) 2>/dev/null; [ -e $out/done ] && echo DONE" </dev/null 2>/dev/null)
    echo "$wanted" | grep -q '^DONE$' && break
    # a window the test opened needs the focus (GNOME holds back new windows of background processes): click it
    for click in $(echo "$wanted" | grep '\.click$'); do
        "$HERE/linuxvm.sh" pointer "$vm" 960 540 >/dev/null && "$HERE/linuxvm.sh" click "$vm" >/dev/null
        "$HERE/linuxvm.sh" ssh "$vm" "rm -f $click" </dev/null
    done
    for want in $(echo "$wanted" | grep '\.want$'); do
        name=$(basename "${want%.want}")
        "$HERE/linuxvm.sh" pointer "$vm" 3 3 >/dev/null 2>&1; "$HERE/linuxvm.sh" pointer "$vm" 5 4 >/dev/null 2>&1; sleep 1
        "$HERE/linuxvm.sh" shot "$vm" "$tmp/$name" && "$HERE/linuxvm.sh" scp "$vm" "$tmp/$name" "${want%.want}" \
            && "$HERE/linuxvm.sh" ssh "$vm" "rm -f $want" </dev/null
    done
    sleep 1
done
rm -rf "$tmp"
