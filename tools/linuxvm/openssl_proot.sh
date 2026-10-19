#!/bin/bash
# openssl_proot.sh <args> - openssl from the Debian 12 rootfs (tools/distro_rootfs.sh) for hosts without one, in the
# current directory: OPENSSL=$PWD/openssl_proot.sh fake_github.py ...
ROOTFS=${ROOTFS:-$HOME/.local/share/distro-rootfs/debian-12}
here=$(cd "$(dirname "$0")/.." && pwd)
cwd=$PWD
quoted=$(printf '%q ' "$@")
PROOT=${PROOT:-$HOME/.local/share/arch-proot/proot} BINDS="-b $cwd:$cwd" \
    "$here/distro_rootfs.sh" run "$ROOTFS" sh -c "cd $cwd && openssl $quoted"
