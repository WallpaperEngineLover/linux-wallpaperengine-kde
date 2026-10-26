#!/bin/bash
# desktop_icons_layout_check.sh <vm> [setup-cmd] - Plasma Wayland: does we_manager's overlay put the icons where Folder View has them,
# for the layout the guest is in right now (setup-cmd, run in the guest session first, changes it: kscreen-doctor scale, panel
# location, ...). Four screenshots of all monitors (spectacle): Folder View with and without desktop files, then the engine
# without and with the overlay; the icon pixels of the first pair and of the second pair (only around where Folder View has them, the wallpaper moves) must cover each other (IoU >= 0.4, the label shadows and the launcher badge differ, and the left, top and right of the bounding boxes within 8 px (label text is thin, its bottom edge is not compared)).
# EXTRA_FILES=N adds N more files so the grid wraps. The overlay runs once per connector in CONNECTORS (default: every enabled output), GDK sorts them its own way, and the VM can
# have a second monitor (LINUXVM_OUTPUTS=2, see linuxvm.sh start).
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 setup=$2
OVERLAY=${OVERLAY:-$HERE/../../../we_manager/src/main/services/desktop-icons-overlay.py}
WP=${WP:-1111948150}
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
tmp=$(mktemp -d)
key=$HOME/.local/share/linuxvm/id_ed25519
port=$(cat "$HOME/.local/share/linuxvm/vms/$vm/port")
shot() {
    sshv '. ~/sessenv; rm -f /tmp/s.png; spectacle -b -f -n -o /tmp/s.png >/dev/null 2>&1'
    scp -q -i "$key" -P "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "lab@127.0.0.1:/tmp/s.png" "$tmp/$1.png"
}
desk=${DESK:-$(sshv 'd=$(xdg-user-dir DESKTOP); mkdir -p "$d"; echo "$d"')}
files() { sshv "d='$desk'; if [ $1 = on ]; then mkdir -p \$d/Projects; touch \$d/notes.txt \$d/data.csv; for n in \$(seq ${EXTRA_FILES:-0}); do touch \$d/file-\$n.txt; done; cp /usr/share/applications/*konsole*.desktop \$d/; else rm -rf \$d/Projects \$d/notes.txt \$d/data.csv \$d/*konsole*.desktop \$d/file-*.txt; fi"; }
stop() { sshv 'pkill -x linux-wallpaper; pkill -f "[o]vl.py"; true'; }

"$HERE/linuxvm.sh" scp "$vm" "$OVERLAY" ovl.py >/dev/null
[ -n "$setup" ] && sshv ". ~/sessenv; $setup"
"$HERE/linuxvm.sh" pointer "$vm" 1000 700 >/dev/null 2>&1
stop; files off; sleep 6; shot r0; files on; sleep 8; shot r
conns=${CONNECTORS:-$(sshv '. ~/sessenv; kscreen-doctor -o | sed "s/\x1b\[[0-9;]*m//g" | sed -n "s/^Output: [0-9]* \([^ ]*\) .*/\1/p"' | tr '\n' ' ' | sed 's/ *$//')}
sshv ". ~/sessenv; cd ~/lwe; (setsid nohup ./lwe-hints/linux-wallpaperengine --screen-root ${conns// / --screen-root } --assets-dir ~/lwe/assets ~/lwe/wp-$WP >/tmp/di-engine.log 2>&1 &); sleep 15"
shot e
lib=$(sshv 'ls /usr/lib64/libgtk4-layer-shell.so.0 /usr/lib/libgtk4-layer-shell.so.0 /usr/lib/x86_64-linux-gnu/libgtk4-layer-shell.so.0 2>/dev/null | head -1')
for c in $conns; do
    sshv ". ~/sessenv; (setsid nohup env LD_PRELOAD=$lib GDK_BACKEND=wayland python3 ~/ovl.py $c >/tmp/ov-$c.log 2>&1 &)"
done
sleep 10
shot o
sshv 'cat /tmp/ov-*.log'
stop; files off
python3 -I -W ignore - "$tmp" <<'P'
import sys
from PIL import Image, ImageChops, ImageFilter
d = sys.argv[1]
im = {n: Image.open(f'{d}/{n}.png').convert('L') for n in ('r0', 'r', 'e', 'o')}
cut = lambda x: x.point(lambda v: 255 if v > 40 else 0)
clean = lambda x: x.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5))
ref = clean(cut(ImageChops.difference(im['r'], im['r0'])))
ovl = clean(cut(ImageChops.difference(im['o'], im['e'])))
box = ref.getbbox() or (0, 0, 1, 1)
box = (max(box[0] - 40, 0), max(box[1] - 40, 0), box[2] + 40, box[3] + 40)
keep = Image.new('L', ref.size, 0)
keep.paste(255, box)
ovl = ImageChops.multiply(ovl, keep)
inter = ImageChops.multiply(ref, ovl).getbbox() and sum(1 for p in ImageChops.multiply(ref, ovl).getdata() if p)
union = sum(1 for p in ImageChops.lighter(ref, ovl).getdata() if p)
r = sum(1 for p in ref.getdata() if p)
o = sum(1 for p in ovl.getdata() if p)
iou = (inter or 0) / union if union else 0
print(f'reference icon px {r}, overlay icon px {o}, IoU {iou:.2f}; bboxes {ref.getbbox()} / {ovl.getbbox()}')
bb1, bb2 = ref.getbbox(), ovl.getbbox()
close = bool(bb1 and bb2) and all(abs(a - b) <= 8 for a, b in zip(bb1[:3], bb2[:3]))
sys.exit(0 if r > 300 and iou >= 0.4 and close else 1)
P
rc=$?
echo "screenshots: $tmp"
[ $rc = 0 ] && echo "PASS $vm layout" || echo "FAIL $vm layout"
exit $rc
