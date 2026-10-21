#!/bin/bash
# ghost_diag.sh <vm> - on a running GNOME VM: what is left of the engine, who holds a connection to the compositor and what
# the extension still tracks. For a VM kept up with KEEP_VM=1 after a frozen wallpaper.
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/linuxvm.sh" ssh "$1" '
echo "== engine, helper and web processes"
ps -eo pid,ppid,stat,etime,cmd | grep -E "[l]inux-wallpaperengine|[w]eb-helper|[g]nome-helper" | cut -c1-170
echo "== sockets to the compositor and their peers"
sudo ss -xp 2>/dev/null | grep -E "wayland|lwe-gnome" | cut -c1-200
echo "== extension state"
gdbus call --session --dest org.linuxwallpaperengine.Shell --object-path /org/linuxwallpaperengine/Shell --method org.linuxwallpaperengine.Shell.GetState 2>&1 | cut -c1-400
' </dev/null
