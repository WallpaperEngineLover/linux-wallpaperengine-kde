#!/usr/bin/env bash
# Checks a release tarball on the running distribution: unpacks it, resolves the engine and the web helper with
# `ldd -r` (portable tarballs through their launcher's --check-libraries) and runs `linux-wallpaperengine --help`.
# A missing library or symbol version fails, and so does an unresolved symbol in our own libraries. Unresolved
# functions in bundled third-party libraries are only listed, they bind lazily.
#
# Usage:
#   tools/smoke_test_release.sh [--install-deps] <tarball>
#
# --install-deps first installs what a desktop system has anyway (GTK 3, NSS, Mesa, libva, PipeWire, CUPS, ALSA,
# GnuTLS, libgcrypt) for bare CI containers, and for a non-portable tarball whatever provides the libraries still
# missing, looked up by soname (LIBNODE_PKG names a built package for Arch's AUR-only libnode). Run as root then.
# Knows Debian/Ubuntu, Fedora, Arch and openSUSE.
set -euo pipefail

install_deps=0
if [ "${1:-}" = --install-deps ]; then
    install_deps=1
    shift
fi
tarball="$(realpath "$1")"

install_desktop_runtime() {
    . /etc/os-release
    case " $ID ${ID_LIKE:-} " in
        *" debian "* | *" ubuntu "*)
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            # Ubuntu 24.04 and Debian 13 renamed some for the 64 bit time_t transition
            local pkgs=() p
            for p in libgtk-3-0 libnss3 libasound2 libcups2 libpipewire-0.3-0 libgl1 libegl1 libgbm1 libvulkan1 \
                libva2 libva-drm2 libva-x11-2 libva-wayland2 libxkbcommon0 libwayland-egl1 libxxf86vm1 liblz4-1 libgnutls30 libgcrypt20; do
                if apt-cache show "${p}t64" >/dev/null 2>&1; then pkgs+=("${p}t64"); else pkgs+=("$p"); fi
            done
            apt-get install -y -qq --no-install-recommends "${pkgs[@]}" >/dev/null
            ;;
        *" fedora "*)
            dnf -y -q install gtk3 nss alsa-lib cups-libs pipewire-libs mesa-libGL mesa-libEGL mesa-libgbm \
                vulkan-loader libva libxkbcommon libwayland-egl libXxf86vm lz4-libs gnutls libgcrypt binutils findutils >/dev/null
            ;;
        *" arch "*)
            pacman -Syu --noconfirm --needed gtk3 nss alsa-lib libcups libpipewire mesa vulkan-icd-loader libva \
                libxkbcommon libxxf86vm lz4 gnutls libgcrypt binutils findutils >/dev/null
            ;;
        *" suse "* | *" opensuse "*)
            zypper -n -q install libgtk-3-0 mozilla-nss libasound2 libcups2 libpipewire-0_3-0 Mesa-libGL1 \
                Mesa-libEGL1 libgbm1 libvulkan1 libva2 libva-drm2 libva-x11-2 libva-wayland2 libxkbcommon0 \
                libwayland-egl1 libXxf86vm1 liblz4-1 libgnutls30 libgcrypt20 binutils findutils gzip tar >/dev/null
            ;;
        *)
            echo "don't know how to install packages on $PRETTY_NAME" >&2
            exit 1
            ;;
    esac
}

# packages providing the given sonames, through each package manager's file index
install_sonames() {
    . /etc/os-release
    local pkgs=() so
    case " $ID ${ID_LIKE:-} " in
        *" debian "* | *" ubuntu "*)
            apt-get install -y -qq --no-install-recommends apt-file >/dev/null
            apt-file update >/dev/null
            for so in "$@"; do
                pkgs+=($(apt-file search -x "/${so//./\\.}\$" | cut -d: -f1 | grep -v -- '-dbg' | head -1))
            done
            apt-get install -y -qq --no-install-recommends "${pkgs[@]}" >/dev/null
            ;;
        *" fedora "*)
            dnf -y -q install "${@/%/()(64bit)}" >/dev/null
            ;;
        *" arch "*)
            if [[ " $* " == *" libnode.so."* ]] && [ -n "${LIBNODE_PKG:-}" ]; then
                pacman -U --noconfirm "$LIBNODE_PKG" >/dev/null
            fi
            pacman -Fy >/dev/null
            for so in "$@"; do
                [[ "$so" == libnode.so.* ]] && continue
                pkgs+=($(pacman -Fq "/usr/lib/$so" | head -1 | cut -d/ -f2))
            done
            [ "${#pkgs[@]}" -gt 0 ] && pacman -S --noconfirm --needed "${pkgs[@]}" >/dev/null
            ;;
        *" suse "* | *" opensuse "*)
            zypper -n -q install "${@/%/()(64bit)}" >/dev/null
            ;;
    esac
}

[ "$install_deps" = 1 ] && install_desktop_runtime

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tar -xzf "$tarball" -C "$work"
dir=("$work"/*/)
dir="${dir[0]%/}"

. /etc/os-release
echo "== $(basename "$tarball") on $PRETTY_NAME, $(ldd --version | head -1)"

failed=0
check_libraries() {
    if [ -e "$dir/linux-wallpaperengine.bin" ]; then
        # the launcher puts the fallback copies the system lacks on the path, like when it starts the engine
        "$dir/linux-wallpaperengine" --check-libraries 2>&1 || true
    else
        ldd -r "$dir/linux-wallpaperengine" "$dir/linux-wallpaperengine-web-helper" 2>&1 || true
    fi
}
out="$(check_libraries)"
if [ "$install_deps" = 1 ] && [ ! -e "$dir/linux-wallpaperengine.bin" ]; then
    mapfile -t sonames < <(awk '/=> not found/ { print $1 }' <<<"$out" | sort -u)
    if [ "${#sonames[@]}" -gt 0 ]; then
        echo "-- installing what provides ${sonames[*]}"
        install_sonames "${sonames[@]}"
        out="$(check_libraries)"
    fi
fi
missing="$(grep -E 'not found' <<<"$out" | sort -u || true)"
ours="$(grep -E 'undefined symbol: .*/(linux-wallpaperengine[^/]*|liblinux-wallpaperengine-lib\.so)\)' <<<"$out" || true)"
lazy="$(grep -E 'undefined symbol' <<<"$out" | grep -vE '/(linux-wallpaperengine[^/]*|liblinux-wallpaperengine-lib\.so)\)' |
    sed -E 's/^\s*undefined symbol: (\S+).*\/([^/)]+)\)$/\2: \1/' | sort -u || true)"
if [ -n "$missing$ours" ]; then
    echo "-- missing"
    sed 's/^/   /' <<<"$missing$ours" | head -30
    failed=1
fi
if [ -n "$lazy" ]; then
    echo "-- unresolved functions in bundled libraries (only fail when called):"
    sed 's/^/   /' <<<"$lazy" | head -30
fi
fallback="$(grep -oE 'lib/fallback/[^/]+' <<<"$out" | sort -u | sed 's|lib/fallback/||' | paste -sd' ' || true)"
[ -n "$fallback" ] && echo "-- uses the bundled fallback for: $fallback"

if ! help="$(cd / && "$dir/linux-wallpaperengine" --help 2>&1)"; then
    echo "-- linux-wallpaperengine --help failed:"
    sed 's/^/   /' <<<"$help" | tail -20
    failed=1
elif ! grep -q -- '--screen-root' <<<"$help"; then
    echo "-- linux-wallpaperengine --help printed something unexpected:"
    sed 's/^/   /' <<<"$help" | head -20
    failed=1
fi

if [ "$failed" = 0 ]; then
    echo "OK"
else
    echo "FAILED"
fi
exit "$failed"
