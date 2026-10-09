#!/usr/bin/env bash
# Packs a finished build into a relocatable tarball: the binary, its library, the web helper, CEF's runtime files
# and the shared kissfft, found through the $ORIGIN runpath `cmake --install` sets. The dependencies' own tools,
# headers and static libs are left out.
#
# Usage:
#   tools/package_release.sh [--portable] <build-dir> <package-name> <output-dir>
#
# Writes <output-dir>/<package-name>.tar.gz with a single <package-name>/ folder inside.
#
# --portable also copies every shared library the binaries need into lib/, except system ones (glibc, GPU,
# display and audio clients, D-Bus, the GTK/NSS stack CEF loads). Build it on the oldest glibc it should run on
# (CI: Debian 12). Bundled libraries calling glibc functions newer than GLIBC_FLOOR (default 2.35) get them from
# tools/glibc_compat. The engine binary becomes linux-wallpaperengine.bin behind tools/portable_launcher.sh, which
# adds the lib/fallback copies the system lacks. lib/SYSTEM_LIBS lists the sonames left to the system,
# tools/smoke_test_release.sh checks a tarball against a distribution.
set -euo pipefail

portable=0
if [ "${1:-}" = --portable ]; then
    portable=1
    shift
fi

tools_dir="$(dirname "$(realpath "$0")")"
build_dir="$(realpath "$1")"
name="$2"
out_dir="$(realpath "$3")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cmake --install "$build_dir" --prefix "$work/install" >/dev/null

pkg="$work/$name"
mkdir -p "$pkg"
cd "$work/install"
cp -a linux-wallpaperengine linux-wallpaperengine-web-helper liblinux-wallpaperengine-lib.so libcef.so libEGL.so \
    libGLESv2.so libvk_swiftshader.so vk_swiftshader_icd.json chrome-sandbox icudtl.dat v8_context_snapshot.bin \
    ./*.pak locales "$pkg/"

# GNUInstallDirs puts kissfft in lib64 on Fedora and lib elsewhere, the runpath covers both
kissfft=(lib*/libkissfft-float.so*)
if [ ! -e "${kissfft[0]}" ]; then
    echo "libkissfft-float.so not found in the install tree" >&2
    exit 1
fi
libdir="$(dirname "${kissfft[0]}")"
[ "$portable" = 1 ] && libdir=lib
mkdir -p "$pkg/$libdir"
cp -a "${kissfft[@]}" "$pkg/$libdir/"

# CEF ships libcef.so with debug symbols, 1.4 GB instead of about 220 MB
strip --strip-unneeded "$pkg/linux-wallpaperengine" "$pkg/linux-wallpaperengine-web-helper" \
    "$pkg/liblinux-wallpaperengine-lib.so" "$pkg/libcef.so"

if [ "$portable" = 1 ]; then
    # sonames (extended regexes) left to the system, everything else gets bundled
    system_libs=(
        # glibc and the compiler runtime
        'ld-linux-x86-64' 'libc' 'libm' 'libdl' 'libpthread' 'librt' 'libresolv' 'libutil' 'libanl' 'libmvec'
        'libBrokenLocale' 'libnsl' 'libstdc\+\+' 'libgcc_s'
        # GPU, display servers, input
        'libGL' 'libGLX' 'libGLdispatch' 'libOpenGL' 'libEGL' 'libGLESv1_CM' 'libGLESv2' 'libgbm' 'libdrm.*'
        # libva: an older bundled one can't load newer drivers
        'libvulkan' 'libva' 'libva-(drm|x11|wayland)' 'libwayland-.*' 'libX11' 'libX11-xcb' 'libXau'
        'libXdmcp' 'libXext' 'libXrender' 'libXfixes' 'libXrandr' 'libXi' 'libXcursor' 'libXinerama' 'libXdamage'
        'libXcomposite' 'libXxf86vm' 'libxkbcommon.*' 'libxshmfence' 'libSM' 'libICE' 'libudev' 'libinput'
        'libevdev'
        # xcb extensions Mesa and cairo use
        'libxcb' 'libxcb-(dri2|dri3|present|sync|xfixes|randr|shm|glx|xkb|render|xinput|composite|damage)'
        # audio and IPC client libraries, they load plugins from system paths
        'libasound' 'libpipewire-0\.3' 'libdbus-1' 'libsystemd' 'libcap'
        # what CEF and the system's GTK stack link themselves
        'libnss3' 'libnssutil3' 'libsmime3' 'libssl3' 'libnspr4' 'libplc4' 'libplds4' 'libcups' 'libatk-1\.0'
        'libatk-bridge-2\.0' 'libatspi' 'libpango.*' 'libcairo.*' 'libpixman-1' 'libgdk_pixbuf-2\.0' 'libgtk-3' 'libgdk-3'
        'libglib-2\.0' 'libgobject-2\.0' 'libgio-2\.0' 'libgmodule-2\.0' 'libgthread-2\.0' 'libffi' 'libpcre2-.*'
        'libmount' 'libblkid' 'libselinux' 'libuuid' 'libz' 'libthai' 'libdatrie' 'libavahi-.*'
    )
    system_re="^($(IFS='|'; echo "${system_libs[*]}"))\.so(\..*)?$"
    # bundled but only used when the system lacks that soname, since system libraries may need their own newer
    # copy (GnuTLS and nettle). The launcher adds lib/fallback/<soname> for the missing ones
    fallback_libs=(
        'libnettle' 'libhogweed' 'libgnutls' 'libtasn1' 'libidn2' 'libunistring' 'libp11-kit' 'libgmp' 'libgcrypt'
        'libgpg-error' 'libssl' 'libcrypto' 'libbrotli(common|dec|enc)' 'liblzma' 'libzstd' 'liblz4' 'libbz2'
        'libexpat' 'libpng16' 'libfreetype' 'libfontconfig' 'libharfbuzz' 'libgraphite2' 'libfribidi' 'libxml2'
        'libkrb5.*' 'libk5crypto' 'libcom_err' 'libgssapi_krb5' 'libkeyutils'
    )
    fallback_re="^($(IFS='|'; echo "${fallback_libs[*]}"))\.so(\..*)?$"

    mkdir -p "$pkg/lib/fallback"
    elves=("$pkg/linux-wallpaperengine" "$pkg/linux-wallpaperengine-web-helper" "$pkg/liblinux-wallpaperengine-lib.so"
        "$pkg"/lib/libkissfft-float.so*)
    declare -A seen=()
    : >"$work/system"
    while [ "${#elves[@]}" -gt 0 ]; do
        elf="${elves[0]}"
        elves=("${elves[@]:1}")
        while read -r soname path; do
            [ -n "${seen[$soname]:-}" ] && continue
            seen[$soname]=1
            if [[ "$soname" =~ $system_re ]] || [ "$soname" = libcef.so ]; then
                echo "$soname" >>"$work/system"
                continue
            fi
            [ -e "$pkg/lib/$soname" ] || [[ "$path" == "$pkg"/* ]] && continue
            if [ "$path" = "not" ]; then
                echo "$elf needs $soname, which is not installed on the build system" >&2
                exit 1
            fi
            dest="$pkg/lib/$soname"
            if [[ "$soname" =~ $fallback_re ]]; then
                mkdir -p "$pkg/lib/fallback/$soname"
                dest="$pkg/lib/fallback/$soname/$soname"
            fi
            cp -L "$path" "$dest"
            chmod u+w "$dest"
            elves+=("$dest")
        done < <(LD_LIBRARY_PATH="$pkg:$pkg/lib:$(find "$pkg/lib/fallback" -mindepth 1 -type d | paste -sd:)" ldd "$elf" |
            awk '$2 == "=>" { print $1, $3 }')
    done
    mapfile -t bundled < <(find "$pkg/lib" -name '*.so*' -type f)

    command -v patchelf >/dev/null || { echo "--portable needs patchelf" >&2; exit 1; }
    # before patchelf, strip can break what patchelf moved around
    strip --strip-unneeded "${bundled[@]}" 2>/dev/null || true

    floor="${GLIBC_FLOOR:-2.35}"
    compat=liblwe-glibc-compat.so
    gcc -shared -fPIC -O2 -o "$pkg/lib/$compat" "$tools_dir/glibc_compat/lwe_glibc_compat.c"
    provided=$(nm -D --defined-only "$pkg/lib/$compat" | awk '$2 == "T" { print $3 }' | paste -sd,)
    python3 "$tools_dir/glibc_compat/retarget_glibc_versions.py" "$floor" "$provided" "${bundled[@]}" >/dev/null
    for f in "${bundled[@]}"; do
        if nm -D --undefined-only "$f" | awk '{ print $2 }' | sed 's/@.*//' | grep -qxF -f <(tr , '\n' <<<"$provided"); then
            patchelf --add-needed "$compat" "$f"
        fi
    done

    for f in "${bundled[@]}"; do
        case "$f" in
            "$pkg"/lib/fallback/*) patchelf --set-rpath '$ORIGIN/../..' "$f" ;;
            *) patchelf --set-rpath '$ORIGIN' "$f" ;;
        esac
    done
    for f in linux-wallpaperengine linux-wallpaperengine-web-helper liblinux-wallpaperengine-lib.so; do
        patchelf --set-rpath '$ORIGIN:$ORIGIN/lib' "$pkg/$f"
    done
    sort -u "$work/system" | grep -v '^libcef.so$' >"$pkg/lib/SYSTEM_LIBS"

    mv "$pkg/linux-wallpaperengine" "$pkg/linux-wallpaperengine.bin"
    cp "$tools_dir/portable_launcher.sh" "$pkg/linux-wallpaperengine"
    chmod 755 "$pkg/linux-wallpaperengine"

    glibc=$(find "$pkg" -maxdepth 4 -type f \( -name '*.so*' -o -name '*.bin' -o -name '*-helper' \) \
        -exec objdump -T {} + 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 || true)
    echo "Bundled $(find "$pkg/lib" -name '*.so*' -type f | wc -l) libraries, needs $glibc"
    if [ "$(printf '%s\n' "${glibc#GLIBC_}" "$floor" | sort -V | tail -1)" != "$floor" ]; then
        echo "needs a newer glibc than $floor:" >&2
        find "$pkg" -maxdepth 4 -type f -name '*.so*' -exec sh -c \
            'objdump -T "$1" | grep -q "($2)" && echo "  $1"' _ {} "$glibc" \; >&2
        exit 1
    fi
    echo "${glibc#GLIBC_}" >"$pkg/lib/GLIBC_VERSION"
fi

mkdir -p "$out_dir"
tar -czf "$out_dir/$name.tar.gz" -C "$work" "$name"
echo "Wrote $out_dir/$name.tar.gz"
