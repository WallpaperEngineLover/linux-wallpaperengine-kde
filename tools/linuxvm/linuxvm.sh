#!/bin/bash
# linuxvm.sh <command> <vm> - Linux desktop VMs (KVM) for what the container can't run: Flatpak (bwrap needs user
# namespaces), a real KMS session of a desktop. A VM is a cloud image overlay set up by cloud-init with user "lab"
# (uid 1000, password "lab", passwordless sudo), reached over ssh on a localhost port. The guest GPU is virtio-vga-gl
# with QEMU's egl-headless display on the container's render node (virgl), so nothing ever shows on the desktop's
# display; screenshots and pointer input go through QMP. No audio device unless started with `desktop`.
#   fetch                 downloads the cloud images (Fedora 44, Arch, Ubuntu 24.04, Debian 13, openSUSE Tumbleweed and Leap 16)
#                         and the installer ISOs (Linux Mint Cinnamon, Manjaro KDE, Pop!_OS, Bazzite)
#   create <vm> <image>   new VM from images/<image>.qcow2 (fedora-44, arch, ubuntu-24.04, debian-13,
#                         opensuse-tumbleweed, opensuse-leap-16), 60G overlay
#   create-iso <vm> <iso> blank 60G disk that boots images/<iso>.iso (a distro installer, e.g. linuxmint-22.3-cinnamon);
#                         remove <vm>/boot.iso after installing. Such a VM has no cloud-init, ssh needs setting up by hand
#   start <vm>            boots it (LINUXVM_CPUS, LINUXVM_MEM, LINUXVM_SIZE=WxH for the display)
#   desktop <vm>          boots it in a window on the container's display (GTK, GL, host PipeWire audio, shared
#                         clipboard with spice-vdagent in the guest) to use it like a normal desktop; same as
#                         LINUXVM_DISPLAY=window start. Stop a headless run of the VM first
#   ssh <vm> [cmd]        runs a command as lab (or a shell)
#   scp <vm> <src> <dst>  copies into the VM (dst relative to /home/lab)
#   shot <vm> <png>       screenshot through the VNC socket (vncshot.py); pointer/click/keys/type/raw go to qmp.py
#   golden <vm>           freezes a stopped VM's disk as golden.qcow2 (and its UEFI vars) and puts a throwaway overlay on top
#   fresh <vm>            throws the overlay away: the next start begins from the golden state again (release_check.sh)
#   a file <vm>/qemu-extra holds extra QEMU arguments (SteamOS: NVMe disk, Valve SMBIOS, the recovery image as USB disk)
#   stop | kill <vm>      ACPI power off | kill QEMU
# QEMU's virtio-gpu and egl-headless modules are not in the image's qemu-system-x86-core: `fetch` unpacks the Fedora
# RPMs (plus virglrenderer) into $LINUXVM/root and QEMU_MODULE_DIR points there.
LINUXVM=${LINUXVM:-$HOME/.local/share/linuxvm}
HERE=$(cd "$(dirname "$0")" && pwd)
QMP="python3 $HERE/../wecompare/winvm/qmp.py"
KEY=$LINUXVM/id_ed25519
ulimit -c 0

vmdir () { echo "$LINUXVM/vms/$1"; }
port () { cat "$(vmdir "$1")/port"; }

ssh_vm () {
    local vm=$1
    shift
    ssh -i "$KEY" -p "$(port "$vm")" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
        -o ConnectTimeout=5 lab@127.0.0.1 "$@"
}

case $1 in
fetch)
    mkdir -p "$LINUXVM/images" "$LINUXVM/rpms" "$LINUXVM/root"
    cd "$LINUXVM/images" || exit 1
    [ -f fedora-44.qcow2 ] || curl -fLo fedora-44.qcow2 \
        https://download.fedoraproject.org/pub/fedora/linux/releases/44/Cloud/x86_64/images/Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2
    [ -f arch.qcow2 ] || curl -fLo arch.qcow2 https://geo.mirror.pkgbuild.com/images/latest/Arch-Linux-x86_64-cloudimg.qcow2
    [ -f ubuntu-24.04.qcow2 ] || curl -fLo ubuntu-24.04.qcow2 \
        https://cloud-images.ubuntu.com/releases/noble/release/ubuntu-24.04-server-cloudimg-amd64.img
    [ -f debian-13.qcow2 ] || curl -fLo debian-13.qcow2 \
        https://cloud.debian.org/images/cloud/trixie/latest/debian-13-genericcloud-amd64.qcow2
    [ -f opensuse-tumbleweed.qcow2 ] || curl -fLo opensuse-tumbleweed.qcow2 \
        https://download.opensuse.org/tumbleweed/appliances/openSUSE-Tumbleweed-Minimal-VM.x86_64-Cloud.qcow2
    [ -f opensuse-leap-16.qcow2 ] || curl -fLo opensuse-leap-16.qcow2 \
        https://download.opensuse.org/distribution/leap/16.0/appliances/Leap-16.0-Minimal-VM.x86_64-Cloud.qcow2
    [ -f linuxmint-22.3-cinnamon.iso ] || curl -fLo linuxmint-22.3-cinnamon.iso \
        https://mirrors.kernel.org/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso
    [ -f manjaro-kde.iso ] || curl -fLo manjaro-kde.iso \
        https://download.manjaro.org/kde/26.1.0/manjaro-kde-26.1.0-260812-linux71.iso
    if [ ! -f pop-os.iso ]; then
        pop=$(curl -fsSL https://api.pop-os.org/builds/24.04/generic)
        curl -fLo pop-os.iso "$(echo "$pop" | python3 -c 'import json,sys; print(json.load(sys.stdin)["url"])')"
        [ "$(sha256sum pop-os.iso | cut -d' ' -f1)" = "$(echo "$pop" | python3 -c 'import json,sys; print(json.load(sys.stdin)["sha_sum"])')" ] || echo "pop-os.iso: sha256 mismatch"
    fi
    [ -f bazzite.iso ] || curl -fLo bazzite.iso https://download.bazzite.gg/bazzite-stable-amd64.iso
    cd "$LINUXVM/rpms" && dnf download --arch x86_64 qemu-device-display-virtio-gpu qemu-device-display-virtio-gpu-gl \
        qemu-device-display-virtio-gpu-pci qemu-device-display-virtio-gpu-pci-gl qemu-device-display-virtio-vga \
        qemu-device-display-virtio-vga-gl qemu-ui-egl-headless qemu-ui-opengl virglrenderer qemu-ui-gtk qemu-audio-pipewire
    cd "$LINUXVM/root" && for rpm in "$LINUXVM"/rpms/*.rpm; do rpm2cpio "$rpm" | cpio -idm 2>/dev/null; done
    [ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N '' -f "$KEY"
    ;;
create)
    vm=$2 image=$LINUXVM/images/$3.qcow2 dir=$(vmdir "$2")
    [ -f "$image" ] || { echo "no $image, run fetch"; exit 1; }
    [ -e "$dir" ] && { echo "$dir exists"; exit 1; }
    mkdir -p "$dir/seed"
    qemu-img create -q -f qcow2 -F qcow2 -b "$image" "$dir/disk.qcow2" 60G
    used=$(cat "$LINUXVM"/vms/*/port 2>/dev/null)
    p=2250
    while echo "$used" | grep -qx $p; do p=$((p + 1)); done
    echo $p > "$dir/port"
    admin=wheel
    case $3 in ubuntu* | debian*) admin=sudo ;; esac
    cat > "$dir/seed/meta-data" <<EOF
instance-id: $vm
local-hostname: $vm
EOF
    cat > "$dir/seed/user-data" <<EOF
#cloud-config
users:
  - name: lab
    uid: "1000"
    groups: [$admin, video, render, input]
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    lock_passwd: false
    plain_text_passwd: lab
    ssh_authorized_keys:
      - $(cat "$KEY.pub")
growpart:
  mode: auto
  devices: ['/']
EOF
    xorriso -as mkisofs -quiet -J -r -V cidata -o "$dir/seed.iso" "$dir/seed"
    cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$dir/vars.fd"
    echo "created $vm (ssh port $p)"
    ;;
create-iso)
    vm=$2 iso=$LINUXVM/images/$3.iso dir=$(vmdir "$2")
    [ -f "$iso" ] || { echo "no $iso"; exit 1; }
    [ -e "$dir" ] && { echo "$dir exists"; exit 1; }
    mkdir -p "$dir"
    qemu-img create -q -f qcow2 "$dir/disk.qcow2" 60G
    ln -s "$iso" "$dir/boot.iso"
    used=$(cat "$LINUXVM"/vms/*/port 2>/dev/null)
    p=2250
    while echo "$used" | grep -qx $p; do p=$((p + 1)); done
    echo $p > "$dir/port"
    cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$dir/vars.fd"
    echo "created $vm (ssh port $p)"
    ;;
start)
    vm=$2 dir=$(vmdir "$2")
    [ -f "$dir/qemu.pid" ] && kill -0 "$(cat "$dir/qemu.pid")" 2>/dev/null && { echo running; exit 0; }
    size=${LINUXVM_SIZE:-1920x1080}
    display=(-display egl-headless,rendernode="${LINUXVM_RENDERNODE:-/dev/dri/renderD128}")
    extra=(-vnc unix:"$dir/vnc.sock")
    if [ "${LINUXVM_DISPLAY:-}" = window ]; then
        display=(-display gtk,gl=on,zoom-to-fit=off,show-menubar=off)
        extra=(-audiodev pipewire,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0
            -device virtio-serial-pci -chardev qemu-vdagent,id=vdagent,clipboard=on,mouse=off
            -device virtserialport,chardev=vdagent,name=com.redhat.spice.0)
    fi
    QEMU_MODULE_DIR=$LINUXVM/root/usr/lib64/qemu LD_LIBRARY_PATH=$LINUXVM/root/usr/lib64 \
    qemu-system-x86_64 -name "$vm" -machine q35 -accel kvm -cpu host -smp "${LINUXVM_CPUS:-8}" -m "${LINUXVM_MEM:-8G}" \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
        -drive if=pflash,format=raw,file="$dir/vars.fd" \
        -drive file="$dir/disk.qcow2",if=virtio,format=qcow2,cache=unsafe,discard=unmap \
        $([ -f "$dir/seed.iso" ] && echo -drive file="$dir/seed.iso",media=cdrom,if=none,id=seed,readonly=on -device ide-cd,drive=seed) \
        $([ -e "$dir/boot.iso" ] && echo -drive file="$dir/boot.iso",media=cdrom,if=none,id=boot,readonly=on -device ide-cd,drive=boot,bootindex=1) \
        -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:"$(port "$vm")"-:22 \
        -device virtio-vga-gl,xres="${size%x*}",yres="${size#*x}" \
        "${display[@]}" "${extra[@]}" \
        -device qemu-xhci -device usb-tablet \
        $([ -f "$dir/qemu-extra" ] && cat "$dir/qemu-extra") \
        -qmp unix:"$dir/qmp.sock",server=on,wait=off -serial file:"$dir/serial.log" \
        -daemonize -pidfile "$dir/qemu.pid"
    [ -f "$dir/seed.iso" ] || { echo "$vm started (no cloud-init, no ssh wait)"; exit 0; }
    for _ in $(seq 120); do ssh_vm "$vm" true 2>/dev/null && { echo "$vm is up"; exit 0; }; sleep 2; done
    echo "$vm: no ssh after 4 minutes, see $dir/serial.log"
    exit 1
    ;;
desktop)
    LINUXVM_DISPLAY=window exec "$0" start "$2"
    ;;
ssh)
    vm=$2
    shift 2
    ssh_vm "$vm" "$@"
    ;;
scp)
    scp -i "$KEY" -P "$(port "$2")" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
        -r "$3" "lab@127.0.0.1:$4"
    ;;
golden)
    dir=$(vmdir "$2")
    [ -f "$dir/qemu.pid" ] && kill -0 "$(cat "$dir/qemu.pid")" 2>/dev/null && { echo "$2 is running, stop it first"; exit 1; }
    [ -f "$dir/golden.qcow2" ] && { echo "$2 has a golden disk already (delete $dir/golden.qcow2 to redo)"; exit 1; }
    mv "$dir/disk.qcow2" "$dir/golden.qcow2" && cp "$dir/vars.fd" "$dir/golden-vars.fd" \
        && qemu-img create -q -f qcow2 -F qcow2 -b "$dir/golden.qcow2" "$dir/disk.qcow2" \
        && echo "$2: golden disk saved, the VM now runs on an overlay"
    ;;
fresh)
    dir=$(vmdir "$2")
    [ -f "$dir/golden.qcow2" ] || { echo "$2 has no golden disk"; exit 1; }
    [ -f "$dir/qemu.pid" ] && kill -0 "$(cat "$dir/qemu.pid")" 2>/dev/null && { echo "$2 is running, stop it first"; exit 1; }
    rm -f "$dir/disk.qcow2" && qemu-img create -q -f qcow2 -F qcow2 -b "$dir/golden.qcow2" "$dir/disk.qcow2" \
        && cp "$dir/golden-vars.fd" "$dir/vars.fd" && echo "$2: back to the golden state"
    ;;
stop)
    $QMP "$(vmdir "$2")/qmp.sock" raw '{"execute":"system_powerdown"}'
    ;;
kill)
    dir=$(vmdir "$2")
    [ -f "$dir/qemu.pid" ] && kill "$(cat "$dir/qemu.pid")"
    ;;
shot)
    python3 "$HERE/vncshot.py" "$(vmdir "$2")/vnc.sock" "$3"
    ;;
pointer | click | keys | type | raw)
    op=$1 vm=$2
    shift 2
    $QMP "$(vmdir "$vm")/qmp.sock" "$op" "$@"
    ;;
*)
    sed -n '2,24p' "$0"
    exit 1
    ;;
esac
