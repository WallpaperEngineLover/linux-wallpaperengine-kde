#!/bin/bash
# ghost_loop.sh <vm> <tries> [extension.js] - ghost_run.sh again and again (tags try1, try2, ...) until a run has a failing
# check or the tries are used up; the same environment variables as ghost_run.sh. A frozen wallpaper after the engine stopped
# (GNOME 46) shows up as the failing hotswap frame check, it needs several full runs to appear.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 tries=$2 ext=$3
[ -n "$vm" ] && [ -n "$tries" ] || { sed -n '2,4p' "$0"; exit 2; }
root=${LWE_REPORT_ROOT:-$HERE/reports/ghost}
for n in $(seq 1 "$tries"); do
    "$HERE/ghost_run.sh" "$vm" "try$n" "$ext"
    if grep -q '^FAIL' "$root/$vm-try$n/$vm/checks.txt" 2>/dev/null; then
        echo "$(date +%T) $vm try$n has a failing check, the loop stops" >> "$root/runs.log"
        break
    fi
done
