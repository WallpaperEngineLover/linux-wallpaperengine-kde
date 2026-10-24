#!/bin/bash
# update_golden_extension.sh <vm>... - puts the repo's GNOME Shell extension (extension.js and metadata.json, the version
# we_manager compares) onto the golden disk of GNOME VMs: boots a fresh overlay, replaces the files, powers off and commits
# the overlay into golden.qcow2 (the UEFI vars are copied as well).
HERE=$(cd "$(dirname "$0")" && pwd)
extdir=${LWE_EXTENSION_DIR:-$HERE/../../gnome-shell-extension/linux-wallpaperengine@wallpaperenginelover.github.io}
uuid=linux-wallpaperengine@wallpaperenginelover.github.io
lv() { "$HERE/linuxvm.sh" "$@"; }
status=0
for vm in "$@"; do
    dir=${LINUXVM:-$HOME/.local/share/linuxvm}/vms/$vm
    [ -f "$dir/golden.qcow2" ] || { echo "$vm: no golden disk"; status=1; continue; }
    lv stop "$vm" >/dev/null 2>&1
    lv fresh "$vm" >/dev/null || { status=1; continue; }
    lv start "$vm" >/dev/null 2>&1
    up=0
    for _ in $(seq 60); do lv ssh "$vm" true </dev/null >/dev/null 2>&1 && { up=1; break; }; sleep 5; done
    if [ $up = 0 ]; then echo "$vm: no ssh"; lv kill "$vm" >/dev/null 2>&1; lv fresh "$vm" >/dev/null; status=1; continue; fi
    lv scp "$vm" "$extdir/extension.js" new-extension.js >/dev/null 2>&1
    lv scp "$vm" "$extdir/metadata.json" new-metadata.json >/dev/null 2>&1
    lv ssh "$vm" "d=~/.local/share/gnome-shell/extensions/$uuid; [ -d \$d ] && cp ~/new-extension.js \$d/extension.js && cp ~/new-metadata.json \$d/metadata.json && rm ~/new-extension.js ~/new-metadata.json && sync && echo copied" </dev/null
    lv ssh "$vm" 'sudo systemctl poweroff' </dev/null >/dev/null 2>&1
    for _ in $(seq 30); do pgrep -f "qemu-system.*-name $vm " >/dev/null || break; sleep 2; done
    lv stop "$vm" >/dev/null 2>&1; sleep 3
    if qemu-img commit -q "$dir/disk.qcow2" 2>&1; then cp "$dir/vars.fd" "$dir/golden-vars.fd"; echo "$vm: golden updated"; else echo "$vm: commit failed"; status=1; fi
    lv fresh "$vm" >/dev/null
done
exit $status
