#!/bin/bash
# winvm.sh <command> - a Windows 11 VM for references only real Windows gives (live WE on Media Foundation, D3D11
# through WARP: there is no GPU in the VM). Needs /dev/kvm in the container (docker-compose.yml) and the evaluation ISO
# (free, 90 days) in $WINVM/iso, see README.md.
#   install          creates the disk and installs Windows unattended (autounattend.xml), ~15 min
#   start            boots the installed VM (WINVM_RESTRICT=off: with internet, only for the evaluation's activation,
#                    slmgr /ato, which an offline VM can't do, the license then reports "expired")
#   stop | kill      shuts Windows down (ACPI) | kills QEMU
#   shot <png>       screenshot; any other qmp.py command works the same way (keys, type, shots, raw)
# Headless: the display is only reachable through QMP/the VNC unix socket in $WINVM/run, never the desktop's display.
# No audio device. The network can't reach anything (restrict=on) except QEMU's SMB share of $WINVM/share, which
# Windows sees as \\10.0.2.4\qemu
WINVM=${WINVM:-$HOME/.local/share/winvm}
RUN=$WINVM/run
HERE=$(cd "$(dirname "$0")" && pwd)
ISO=${WINVM_ISO:-$(ls $WINVM/iso/*.iso 2>/dev/null | head -1)}
mkdir -p $RUN $WINVM/share $WINVM/tpm
ulimit -c 0

tpm () {
    pgrep -f "swtpm socket.*$WINVM/tpm" >/dev/null && return
    swtpm socket --tpmstate dir=$WINVM/tpm --ctrl type=unixio,path=$RUN/tpm.sock --tpm2 --daemon \
        --log file=$RUN/swtpm.log
    sleep 0.5
}

qemu () {
    tpm
    [ -f $WINVM/vars.fd ] || cp /usr/share/edk2/ovmf/OVMF_VARS.fd $WINVM/vars.fd
    qemu-system-x86_64 -name winvm -machine q35 -accel kvm -cpu host -smp ${WINVM_CPUS:-8} -m ${WINVM_MEM:-8G} \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
        -drive if=pflash,format=raw,file=$WINVM/vars.fd \
        -chardev socket,id=tpm,path=$RUN/tpm.sock -tpmdev emulator,id=tpm0,chardev=tpm -device tpm-tis,tpmdev=tpm0 \
        -device ahci,id=ahci -drive id=disk,file=$WINVM/disk.qcow2,if=none,format=qcow2,cache=unsafe \
        -device ide-hd,drive=disk,bus=ahci.0 \
        -nic user,model=e1000e,restrict=${WINVM_RESTRICT:-on},smb=$WINVM/share \
        -vga std -device qemu-xhci -device usb-tablet -display none -vnc unix:$RUN/vnc.sock \
        -qmp unix:$RUN/qmp.sock,server=on,wait=off -rtc base=localtime \
        -daemonize -pidfile $RUN/qemu.pid "$@"
}

case $1 in
install)
    [ -f "$ISO" ] || { echo "no ISO in $WINVM/iso"; exit 1; }
    [ -f $WINVM/disk.qcow2 ] && { echo "$WINVM/disk.qcow2 exists, remove it to reinstall"; exit 1; }
    qemu-img create -f qcow2 $WINVM/disk.qcow2 ${WINVM_DISK:-80G} >/dev/null
    rm -f $WINVM/vars.fd $WINVM/tpm/*
    mkdir -p $RUN/unattend && cp $HERE/autounattend.xml $HERE/firstlogon.ps1 $RUN/unattend/
    xorriso -as mkisofs -quiet -J -r -V UNATTEND -o $RUN/unattend.iso $RUN/unattend
    qemu -drive file="$ISO",media=cdrom,if=none,id=cd0,readonly=on -device ide-cd,drive=cd0,bus=ahci.1,bootindex=0 \
        -drive file=$RUN/unattend.iso,media=cdrom,if=none,id=cd1,readonly=on -device ide-cd,drive=cd1,bus=ahci.2
    for i in $(seq 20); do python3 $HERE/qmp.py $RUN/qmp.sock keys ret 2>/dev/null; sleep 0.5; done
    echo "installing, watch with: $0 shot /tmp/x.png"
    ;;
start)
    [ -f $RUN/qemu.pid ] && kill -0 $(cat $RUN/qemu.pid) 2>/dev/null && { echo running; exit 0; }
    qemu
    ;;
stop)
    python3 $HERE/qmp.py $RUN/qmp.sock raw '{"execute":"system_powerdown"}'
    ;;
kill)
    [ -f $RUN/qemu.pid ] && kill $(cat $RUN/qemu.pid)
    pkill -f "swtpm socket.*$WINVM/tpm"
    ;;
bootstrap)
    # once after install: types the agent installer into Win+R, from then on "run" works
    cp $HERE/agent.ps1 $HERE/install_agent.ps1 $WINVM/share/
    python3 $HERE/qmp.py $RUN/qmp.sock keys meta_l-r
    sleep 2
    python3 $HERE/qmp.py $RUN/qmp.sock type 'powershell -ExecutionPolicy Bypass -File \\10.0.2.4\qemu\install_agent.ps1'
    python3 $HERE/qmp.py $RUN/qmp.sock keys ret
    ;;
launch)
    # launch <exe> [args] [workdir]: starts a program on the VM's desktop through a one-off scheduled task, so it
    # doesn't inherit the agent's output handles (the agent would wait for it to exit)
    EXE=$2 ARGS=${3:-} DIR=${4:-}
    [ -n "$DIR" ] && DIR="'$DIR'" || DIR="(Split-Path '$EXE')"
    exec bash "$HERE/winvm.sh" run -c "\$a = New-ScheduledTaskAction -Execute '$EXE' -Argument '$ARGS' -WorkingDirectory $DIR; Register-ScheduledTask -TaskName winvm-launch -Action \$a -Force | Out-Null; Start-ScheduledTask winvm-launch" 60
    ;;
run)
    # run <script.ps1 | -c 'command'> [timeout s]: runs it in the VM through the agent, prints its output
    NAME=job$$_$(date +%s%N)
    mkdir -p $WINVM/share/cmd $WINVM/share/out
    if [ "$2" = -c ]; then printf '%s\r\n' "$3" > $WINVM/share/cmd/$NAME.tmp; T=${4:-600}; else cp "$2" $WINVM/share/cmd/$NAME.tmp; T=${3:-600}; fi
    mv $WINVM/share/cmd/$NAME.tmp $WINVM/share/cmd/$NAME.ps1
    timeout $T bash -c "until [ -f $WINVM/share/out/$NAME.done ]; do sleep 0.5; done" || echo "timed out" >&2
    cat $WINVM/share/out/$NAME.txt 2>/dev/null | iconv -f UTF-16LE -t UTF-8 2>/dev/null | tr -d '\r\357\273\277'
    rm -f $WINVM/share/out/$NAME.*
    ;;
*)
    python3 $HERE/qmp.py $RUN/qmp.sock "$@"
    ;;
esac
