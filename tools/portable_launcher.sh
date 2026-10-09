#!/bin/sh
# linux-wallpaperengine of the portable release (tools/package_release.sh --portable). Starts the engine with the
# bundled lib/fallback/<soname> copies on the library path, only for sonames the system doesn't have.
#
# --check-libraries prints `ldd -r` of the engine and the web helper with that path and exits.
here=$(dirname "$(readlink -f "$0")")

system_has() {
    for ldconfig in /sbin/ldconfig /usr/sbin/ldconfig /usr/bin/ldconfig; do
        if [ -x "$ldconfig" ]; then
            "$ldconfig" -p 2>/dev/null | grep -q "^[[:space:]]*$1 (.*x86-64" && return 0
            break
        fi
    done
    for libdir in /lib64 /usr/lib64 /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib /lib; do
        [ -e "$libdir/$1" ] && return 0
    done
    return 1
}

fallback=
for dir in "$here"/lib/fallback/*/; do
    [ -d "$dir" ] || continue
    soname=$(basename "$dir")
    system_has "$soname" || fallback="$fallback:${dir%/}"
done
if [ -n "$fallback" ]; then
    LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH}$fallback"
    LD_LIBRARY_PATH="${LD_LIBRARY_PATH#:}"
    export LD_LIBRARY_PATH
fi

if [ "$1" = --check-libraries ]; then
    ldd -r "$here/linux-wallpaperengine.bin" "$here/linux-wallpaperengine-web-helper"
    exit
fi

exec "$here/linux-wallpaperengine.bin" "$@"
