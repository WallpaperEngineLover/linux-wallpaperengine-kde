#!/bin/bash
# ghost_run.sh <vm> <tag> [extension.js] - one full release_check of a GNOME VM with a different GNOME Shell extension than
# the one on its golden disk (the repo's, or a debug_extension.py build): the extension goes onto the VM's overlay, which is
# thrown away at the end, so the golden disk stays as it is. The Shell's LWE-DBG journal lines (debug builds), a final
# screenshot and the release_check report land in <report root>/<vm>-<tag>/, a summary line in <report root>/runs.log.
#   LWE_CANDIDATE      the unpacked candidate dir (with lwe-hints/), required
#   LWE_ENGINE_TARBALL the candidate's tarball, required
#   LWE_APP_DEB        we_manager's .deb, for profiles with WEM=deb
#   LWE_APP_DIR        we_manager's unpacked app, for the others
#   LWE_REPORT_ROOT    default reports/ghost
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 tag=$2
[ -n "$vm" ] && [ -n "$tag" ] || { sed -n '2,3p' "$0"; exit 2; }
ext=${3:-$HERE/../../gnome-shell-extension/linux-wallpaperengine@wallpaperenginelover.github.io/extension.js}
root=${LWE_REPORT_ROOT:-$HERE/reports/ghost}
out=$root/$vm-$tag
lv() { "$HERE/linuxvm.sh" "$@"; }
sshv() { lv ssh "$vm" "$@" </dev/null; }
uuid=linux-wallpaperengine@wallpaperenginelover.github.io

if grep -q '^WEM=deb' "$HERE/profiles/$vm.sh"; then app=$LWE_APP_DEB; else app=$LWE_APP_DIR; fi
[ -n "$LWE_CANDIDATE" ] && [ -n "$LWE_ENGINE_TARBALL" ] && [ -n "$app" ] || { echo "LWE_CANDIDATE, LWE_ENGINE_TARBALL and the app of this profile's kind are required"; exit 2; }

mkdir -p "$out"
lv fresh "$vm" >/dev/null 2>&1
lv start "$vm" >/dev/null 2>&1
for _ in $(seq 60); do sshv true 2>/dev/null && break; sleep 5; done
lv scp "$vm" "$ext" dbg-extension.js >/dev/null 2>&1
sshv "cp ~/dbg-extension.js ~/.local/share/gnome-shell/extensions/$uuid/extension.js"
sshv 'sync; sudo systemctl poweroff' >/dev/null 2>&1
sleep 25
lv stop "$vm" >/dev/null 2>&1
sleep 5

KEEP_VM=1 REPORT_DIR=$out "$HERE/release_check.sh" --vm "$vm" --no-overlay --candidate "$LWE_CANDIDATE" --app "$app" \
    --engine-tarball "$LWE_ENGINE_TARBALL" > "$out.out" 2>&1
sshv "journalctl -b --no-pager -o short-monotonic 2>/dev/null | grep -E 'LWE-DBG|stack_position|Buggy client'" > "$out/shell-debug.log" 2>&1
lv shot "$vm" "$out/final.png" >/dev/null 2>&1
sshv 'sudo systemctl poweroff' >/dev/null 2>&1
sleep 20
lv stop "$vm" >/dev/null 2>&1
lv fresh "$vm" >/dev/null 2>&1
echo "$(date +%T) $vm $tag: $(grep -c . "$out/shell-debug.log") debug lines, $(grep -c '^FAIL' "$out/$vm/checks.txt") FAIL, $(grep -c 'hiding orphan' "$out/shell-debug.log") orphans hidden" >> "$root/runs.log"
