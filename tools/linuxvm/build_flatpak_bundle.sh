#!/bin/bash
# build_flatpak_bundle.sh <we_manager .deb> <engine portable tarball> <out.flatpak> [vm] - builds we_manager's Flatpak (the manifest in
# $WEM_DIR/packaging/flatpak, default /workspace/we_manager) inside a VM, because flatpak-builder needs user namespaces and the dev
# sandbox has none. Boots the VM from its golden disk (one of the shared slots), installs flatpak-builder, fetches the 25.08 runtime and the
# Electron BaseApp from Flathub, builds with --disable-rofiles-fuse (rofiles-fuse cannot mount in the VM), writes the bundle to <out> and
# powers the VM off. The release gate takes it as `release_check.sh --flatpak <out>`. About 8 minutes.
HERE=$(cd "$(dirname "$0")" && pwd)
deb=$1 engine=$2 out=$3 VM=${4:-fedora-gnome}
WEM_DIR=${WEM_DIR:-/workspace/we_manager}
[ -f "$deb" ] && [ -f "$engine" ] && [ -n "$out" ] || { sed -n '2,7p' "$0"; exit 2; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$WEM_DIR"/packaging/flatpak/{io.github.WallpaperEngineLover.we_manager.yml,we-manager.sh,io.github.WallpaperEngineLover.we_manager.metainfo.xml} "$work/"
cp "$deb" "$work/we-manager.deb"
cp "$engine" "$work/linux-wallpaperengine-kde-portable-x86_64.tar.gz"

. "$HERE/vm_slots.sh"
lv() { "$HERE/linuxvm.sh" "$@"; }
sshv() { lv ssh "$VM" "$@" </dev/null; }
take_slot
status=1
lv fresh "$VM"
lv start "$VM"
for _ in $(seq 60); do
    lv scp "$VM" "$HERE/sessenv.sh" lwe/sessenv.sh 2>/dev/null && sshv "bash ~/lwe/sessenv.sh" >/dev/null 2>&1 && break
    sleep 5
done
sshv 'rm -rf ~/fpbuild; mkdir -p ~/fpbuild'
for f in "$work"/*; do lv scp "$VM" "$f" "fpbuild/$(basename "$f")"; done
sshv 'sudo dnf install -y flatpak-builder >/tmp/dnf.log 2>&1 || tail -3 /tmp/dnf.log'
sshv 'flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
cd ~/fpbuild && flatpak-builder --user --disable-rofiles-fuse --install-deps-from=flathub --force-clean --repo=repo build-dir io.github.WallpaperEngineLover.we_manager.yml >/tmp/fpb.log 2>&1 || { tail -15 /tmp/fpb.log; exit 1; }
flatpak build-bundle repo we-manager.flatpak io.github.WallpaperEngineLover.we_manager' && {
    sshv 'cat ~/fpbuild/we-manager.flatpak' > "$out" && [ -s "$out" ] && status=0
}
sshv "sudo systemctl poweroff" >/dev/null 2>&1
release_slot
[ $status = 0 ] && echo "bundle: $out ($(du -h "$out" | cut -f1))" || echo "FAIL: no bundle"
exit $status
