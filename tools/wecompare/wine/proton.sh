#!/bin/bash
# Proton builds for live WE, next to the system Wine rig (README "Proton")
#   proton.sh available [ge|cachyos|valve]   releases that can be installed
#   proton.sh install <name>                 GE-Proton11-9, cachyos-11.0-20261005-slr, proton-experimental (Valve)
#   proton.sh list                           installed builds
#   proton.sh env <name>                     WE_WINE_DIR/WE_PREFIX for the we_*.sh scripts (also WE_PROTON=<name>)
#   proton.sh remove <name>
#   proton.sh i686                           private i686 glibc for images without /lib/ld-linux.so.2 (see below)
# Each build gets a shim dir ($PROTON_DIR/<name>/shim/bin/wine, wineserver): wine goes through Proton's `run`
# (proton_run.py), so WE sees the environment and prefix setup (DXVK, vkd3d-proton, fonts, wine-mono) Steam would
# give it, minus the Steam Linux Runtime container (no user namespaces here, host libraries are used).
# The prefix is $WE_LIVE_DIR/proton/<name>/pfx, created on the first wine call. Setup output and Proton's own
# messages go to the caller's stderr like Wine's.
# Proton's wine always starts through the i386 preloader (also for 64-bit programs), which needs the 32-bit
# /lib/ld-linux.so.2. The image has glibc.i686 for that; without it the shim runs Proton under proot with a private
# copy bound there (`proton.sh i686` fetches it, ptrace overhead, so timings are off).
S=${WE_LIVE_DIR:-$HOME/.local/share/we_live}
HERE=$(cd "$(dirname "$0")" && pwd)
P=${PROTON_DIR:-$HOME/.local/share/proton}
mkdir -p "$P/dl"

gh_releases() {
    curl -fsSL "https://api.github.com/repos/$1/releases?per_page=${2:-15}"
}

shim() {
    local name=$1 root=$P/$1
    mkdir -p "$root/shim/bin" "$P/steam"
    cat > "$root/shim/bin/wine" <<SH
#!/bin/bash
# WINEPREFIX=<compat dir>/pfx as the we_*.sh scripts set it from WE_PREFIX
export STEAM_COMPAT_DATA_PATH=\${WINEPREFIX%/pfx} STEAM_COMPAT_CLIENT_INSTALL_PATH=$P/steam
export SteamAppId=\${SteamAppId:-431960} SteamGameId=\${SteamGameId:-431960}
mkdir -p "\$STEAM_COMPAT_DATA_PATH"
# the image's global d3d overrides would hide Proton's own; WE_WINED3D=1 sets dxgi=b, Proton's wined3d switch
case "\$WINEDLLOVERRIDES" in *dxgi=b*) export PROTON_USE_WINED3D=1 ;; esac
unset WINEDLLOVERRIDES
# xalia (gamepad UI navigation, a mono process per prefix) has nothing to do with rendering
export PROTON_USE_XALIA=\${PROTON_USE_XALIA:-0}
run=()
if [ ! -e /lib/ld-linux.so.2 ]; then
    [ -e "$P/i686/root/usr/lib/ld-linux.so.2" ] || { echo "no /lib/ld-linux.so.2, run proton.sh i686" >&2; exit 1; }
    export LD_LIBRARY_PATH=\${LD_LIBRARY_PATH:+\$LD_LIBRARY_PATH:}$P/i686/root/usr/lib
    run=(proot -b "$P/i686/root/usr/lib/ld-linux.so.2:/lib/ld-linux.so.2")
fi
# Proton's "run" path (prefix setup, DXVK overrides) with the program started directly, see proton_run.py
exec "\${run[@]}" python3 "$HERE/proton_run.py" "$root/proton" "\$@"
SH
    cat > "$root/shim/bin/wineserver" <<SH
#!/bin/bash
exec "$root/files/bin/wineserver" "\$@"
SH
    chmod +x "$root/shim/bin/wine" "$root/shim/bin/wineserver"
}

case $1 in
available)
    case ${2:-all} in ge|all)
        echo "# GE-Proton (GloriousEggroll/proton-ge-custom)"
        gh_releases GloriousEggroll/proton-ge-custom | python3 -c '
import json, sys
for r in json.load(sys.stdin):
    if any(a["name"].endswith("x86_64.tar.gz") for a in r["assets"]): print(r["tag_name"], r["published_at"][:10])'
    esac
    case ${2:-all} in cachyos|all)
        echo "# proton-cachyos (CachyOS/proton-cachyos)"
        gh_releases CachyOS/proton-cachyos | python3 -c '
import json, sys
for r in json.load(sys.stdin):
    if any(a["name"].endswith("-x86_64.tar.xz") for a in r["assets"]): print(r["tag_name"], r["published_at"][:10])'
    esac
    case ${2:-all} in valve|all)
        echo "# Valve Proton (current build of each branch, all but experimental need STEAM_USER)"
        printf '%s\n' proton-experimental proton-11 proton-10 proton-9 proton-8 proton-7 proton-hotfix
    esac
    ;;
install)
    name=$2; [ -n "$name" ] || { echo "usage: proton.sh install <name>"; exit 1; }
    [ -x "$P/$name/proton" ] && { shim "$name"; echo "$name already installed"; exit 0; }
    case $name in
    GE-Proton*)
        base=https://github.com/GloriousEggroll/proton-ge-custom/releases/download/$name
        file=$name-x86_64.tar.gz; sum=$name-x86_64.sha512sum
        ;;
    cachyos-*)
        base=https://github.com/CachyOS/proton-cachyos/releases/download/$name
        file=proton-$name-x86_64.tar.xz; sum=proton-$name-x86_64.sha512sum
        ;;
    proton-*)
        exec bash "$(dirname "$0")/proton_valve.sh" "$name"
        ;;
    *) echo "unknown build $name (see proton.sh available)"; exit 1 ;;
    esac
    cd "$P/dl" || exit 1
    curl -fL --progress-bar -o "$file" "$base/$file" && curl -fsSL -o "$sum" "$base/$sum" || exit 1
    sha512sum -c "$sum" || exit 1
    tmp=$(mktemp -d "$P/dl/x.XXXX")
    tar -xf "$file" -C "$tmp" || exit 1
    top=$(ls "$tmp"); [ "$(echo "$top" | wc -l)" = 1 ] || { echo "unexpected archive layout"; exit 1; }
    mv "$tmp/$top" "$P/$name" && rmdir "$tmp" && rm -f "$file"
    shim "$name"
    echo "installed $P/$name ($(cat "$P/$name/version"))"
    ;;
list)
    for d in "$P"/*/; do
        [ -x "$d/proton" ] || continue
        n=$(basename "$d"); printf '%-36s %s' "$n" "$(cut -d' ' -f2- "$d/version" 2>/dev/null)"
        [ -d "$S/proton/$n/pfx" ] && printf '  (prefix)'
        echo
    done
    ;;
env)
    name=$2; [ -x "$P/$name/proton" ] || { echo "echo 'proton.sh: $name is not installed' >&2; exit 1"; exit 1; }
    [ -x "$P/$name/shim/bin/wine" ] || shim "$name"
    echo "export WE_WINE_DIR=$P/$name/shim WE_PREFIX=$S/proton/$name/pfx"
    ;;
i686)
    mkdir -p "$P/i686/rpms" "$P/i686/root" && cd "$P/i686/rpms" || exit 1
    dnf download --setopt=cachedir="$P/i686/dnfcache" --arch=i686 glibc libgcc libstdc++ || exit 1
    cd "$P/i686/root" && for r in "$P"/i686/rpms/*.rpm; do rpm2cpio "$r" | cpio -idm --quiet; done
    rm -rf "$P/i686/dnfcache"
    ;;
remove)
    name=$2; [ -n "$name" ] && [ -d "$P/$name" ] || { echo "not installed"; exit 1; }
    rm -rf "${P:?}/$name" "${S:?}/proton/$name"
    ;;
*)
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
