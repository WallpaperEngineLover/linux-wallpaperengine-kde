#!/bin/bash
# desktop_icons_check.sh <vm> [screen] - the desktop's icons must stay visible above the engine's wallpaper, on a running VM
# (release_check.sh runs it for every profile). Screenshots through the VM's display: A the desktop, B the same with a few
# files on the desktop folder (what the icon provider of the session draws is where B differs from A, the mask), E the engine
# running without the files, D the engine with them. The icons are visible when D differs from E over the mask about as much
# as B differs from A. No provider in the session (GNOME without Desktop Icons NG, wlroots compositors, ...) is a SKIP, on
# Plasma it is a FAIL. Plasma on Wayland hides Folder View under the engine's bottom layer surface, there we_manager's
# overlay (OVERLAY, default the sibling checkout's script) is installed with what it needs (gtk4-layer-shell, PyGObject,
# pycairo) and started after the engine, as we_manager does.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 screen=${2:-Virtual-1}
OVERLAY=${OVERLAY:-$HERE/../../../we_manager/src/main/services/desktop-icons-overlay.py}
WP=${WP:-1111948150}
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $vm: $1"; }
fail() { echo "FAIL $vm: $1"; status=1; }
skip() { echo "SKIP $vm: $1"; }
tmp=$(mktemp -d)
shot() { "$HERE/linuxvm.sh" shot "$vm" "$tmp/$1.png" >/dev/null 2>&1; }

for _ in $(seq 60); do sshv 'bash ~/lwe/sessenv.sh' >/dev/null 2>&1 && break; sleep 5; done
sshv 'bash ~/lwe/sessenv.sh' >/dev/null 2>&1 || { fail "no desktop session"; exit 1; }
session=$(sshv '. ~/sessenv; echo "$XDG_CURRENT_DESKTOP $XDG_SESSION_TYPE"')
kde=0; [[ $session == *KDE*wayland* ]] && kde=1
desk=$(sshv 'd=$(xdg-user-dir DESKTOP 2>/dev/null); [ -n "$d" ] && [ "$d" != "$HOME" ] || d=$HOME/Desktop; mkdir -p "$d"; echo "$d"')

make_files() {
    sshv "mkdir -p '$desk/Projects'; touch '$desk/notes.txt' '$desk/data.csv'; find /usr/share/applications \\( -name '*konsole*.desktop' -o -name 'org.gnome.Nautilus.desktop' -o -name 'firefox*.desktop' \\) | head -2 | xargs -I{} cp {} '$desk/'; true"
}
remove_files() {
    sshv "rm -rf '$desk/Projects' '$desk/notes.txt' '$desk/data.csv'; find '$desk' -maxdepth 1 \\( -name '*konsole*.desktop' -o -name 'org.gnome.Nautilus.desktop' -o -name 'firefox*.desktop' \\) -delete; true"
}
cleanup() { sshv 'pkill -f "[o]vl.py"; pkill -x linux-wallpaper; true'; remove_files; }

sshv 'pkill -f "[o]vl.py"; pkill -x linux-wallpaper; true'
remove_files
sleep 4
shot A
make_files
sleep 8
shot B
remove_files
sleep 4
shot A2

# icon pixels: where B differs from the empty desktop both times, a notification that came up and stayed is in B and A2
mask=$(python3 -W ignore - "$tmp" <<'E'
import sys
from PIL import Image, ImageChops
d = sys.argv[1]
a, a2, b = (Image.open(f'{d}/{n}.png').convert('L') for n in ('A', 'A2', 'B'))
cut = lambda x: x.point(lambda v: 255 if v > 40 else 0)
diff = ImageChops.multiply(cut(ImageChops.difference(a, b)), cut(ImageChops.difference(a2, b)))
diff.save(f'{d}/mask.png')
print(sum(1 for p in diff.getdata() if p))
E
)
if [ "${mask:-0}" -lt 300 ]; then
    cleanup
    if [ $kde = 1 ]; then fail "Plasma's Folder View shows no icons for files on $desk (mask $mask px, screenshots in $tmp)"; else
        skip "no desktop icon provider in this session ($session): nothing to keep above the wallpaper"
    fi
    exit $status
fi
pass "the session draws its icons ($mask px differ with files on the desktop)"

sshv ". ~/sessenv; cd ~/lwe; (setsid nohup ./lwe-hints/linux-wallpaperengine ${ENGINE_ARGS:-} --screen-root $screen --assets-dir ~/lwe/assets ~/lwe/wp-$WP >/tmp/di-engine.log 2>&1 &); sleep 20"
shot E

if [ $kde = 1 ]; then
    "$HERE/linuxvm.sh" scp "$vm" "$OVERLAY" ovl.py >/dev/null 2>&1 || { fail "overlay script not found: $OVERLAY"; cleanup; exit 1; }
    libs='bash -c "ls /usr/lib64/libgtk4-layer-shell.so* /usr/lib/libgtk4-layer-shell.so* /usr/lib/x86_64-linux-gnu/libgtk4-layer-shell.so* 2>/dev/null | head -1"'
    if [ -z "$(sshv "$libs")" ]; then
        sshv 'if command -v dnf >/dev/null; then sudo dnf install -y gtk4-layer-shell python3-gobject python3-cairo
              elif command -v pacman >/dev/null; then sudo pacman -Sy --noconfirm --needed gtk4-layer-shell python-gobject python-cairo
              elif command -v apt-get >/dev/null; then sudo apt-get install -y gir1.2-gtk4layershell-1.0 libgtk4-layer-shell0 python3-gi python3-gi-cairo gir1.2-gtk-4.0
              elif command -v zypper >/dev/null; then sudo zypper -n install libgtk4-layer-shell0 typelib-1_0-Gtk4LayerShell-1_0 python3-gobject python3-cairo typelib-1_0-Gtk-4_0
              fi' >"$tmp/install.log" 2>&1
    fi
    lib=$(sshv "$libs")
    if [ -z "$lib" ]; then
        skip "gtk4-layer-shell can't be installed here ($(tail -1 "$tmp/install.log" | cut -c1-100)), the overlay can't run"
        cleanup; exit $status
    fi
    preload=$(sshv 'bash -c "ls /usr/lib64/libgtk4-layer-shell.so.0 /usr/lib/libgtk4-layer-shell.so.0 /usr/lib/x86_64-linux-gnu/libgtk4-layer-shell.so.0 2>/dev/null | head -1"')
    [ -n "$preload" ] || preload=$lib
    make_files
    # gtk4-layer-shell 1.0.x dlopens the plain libwayland-client.so (-devel only), we_manager points LD_LIBRARY_PATH at a symlink
    sshv 'mkdir -p ~/ovl-lib; for l in /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu; do [ -e $l/libwayland-client.so.0 ] && ln -sf $l/libwayland-client.so.0 ~/ovl-lib/libwayland-client.so && break; done; true'
    sshv ". ~/sessenv; (setsid nohup env LD_LIBRARY_PATH=\$HOME/ovl-lib LD_PRELOAD=$preload GDK_BACKEND=wayland python3 ~/ovl.py $screen >/tmp/di-overlay.log 2>&1 &); sleep 8"
    [ "$(sshv 'pgrep -fc "python3 .*[o]vl.py"')" -ge 1 ] && pass "overlay running" || fail "overlay exited: $(sshv 'head -5 /tmp/di-overlay.log')"
else
    make_files
    sleep 8
fi
shot D

if python3 -W ignore - "$tmp" <<'E'
import sys
from PIL import Image, ImageChops
d = sys.argv[1]
im = {n: Image.open(f'{d}/{n}.png').convert('L') for n in 'ABDE'}
mask = Image.open(f'{d}/mask.png')
def mean(x, y):
    diff = ImageChops.difference(x, y)
    px = [p for p, m in zip(diff.getdata(), mask.getdata()) if m]
    return sum(px) / len(px)
provider = mean(im['A'], im['B'])
kept = mean(im['E'], im['D'])
print(f'icon area: {provider:.1f} levels without the engine, {kept:.1f} with it')
sys.exit(0 if kept > provider * 0.4 else 1)
E
then pass "icons visible above the wallpaper"; else fail "icons hidden by the wallpaper (screenshots in $tmp)"; fi

cleanup
echo "     screenshots: $tmp"
exit $status
