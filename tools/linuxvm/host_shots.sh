#!/bin/bash
# host_shots.sh <vm> <guest out dir> [seconds] - serves the "SHOT_CMD=host" requests of guest_test.sh: whenever the guest
# creates <file>.png.want, takes a screenshot of the VM's display (the same VNC socket `linuxvm.sh shot` uses) and copies
# it into the guest as <file>.png. Runs until the seconds (default 900) are over or the guest's test run has finished
# (the guest creates <out dir>/done).
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 out=$2 limit=${3:-900}
tmp=$(mktemp -d)
end=$((SECONDS + limit))
while [ $SECONDS -lt $end ]; do
    wanted=$("$HERE/linuxvm.sh" ssh "$vm" "ls $out/*.want 2>/dev/null; [ -e $out/done ] && echo DONE" </dev/null 2>/dev/null)
    echo "$wanted" | grep -q '^DONE$' && break
    for want in $(echo "$wanted" | grep '\.want$'); do
        name=$(basename "${want%.want}")
        "$HERE/linuxvm.sh" shot "$vm" "$tmp/$name" && "$HERE/linuxvm.sh" scp "$vm" "$tmp/$name" "${want%.want}" \
            && "$HERE/linuxvm.sh" ssh "$vm" "rm -f $want" </dev/null
    done
    sleep 1
done
rm -rf "$tmp"
