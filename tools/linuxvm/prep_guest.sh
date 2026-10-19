#!/bin/bash
# prep_guest.sh <vm> [candidate dir] - fills ~/lwe in a VM the way release_check.sh expects it: test assets and wallpapers
# (kept in $LINUXVM/payload), the engine under test (candidate/lwe-hints), the guest scripts. Needs working ssh to the VM.
HERE=$(cd "$(dirname "$0")" && pwd)
LINUXVM=${LINUXVM:-$HOME/.local/share/linuxvm}
vm=$1 cand=${2:-/tmp/claude-1000/pkgout/cand}
lv() { "$HERE/linuxvm.sh" "$@"; }
lv ssh "$vm" 'mkdir -p ~/lwe' </dev/null
for d in "$LINUXVM"/payload/* "$cand/lwe-hints"; do lv scp "$vm" "$d" lwe/; done
for f in sessenv.sh guest_test.sh fullscreen_window.py maximize_kde.sh; do lv scp "$vm" "$HERE/$f" "lwe/$f"; done
lv ssh "$vm" 'du -sh ~/lwe' </dev/null
