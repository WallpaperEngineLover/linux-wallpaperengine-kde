#!/usr/bin/env bash
# Packs a finished build into a relocatable tarball: the binary, its library, CEF's runtime files
# and the shared kissfft, all found through the $ORIGIN runpath `cmake --install` sets. The install
# tree also carries the dependencies' own tools, headers and static libs, those are left out.
#
# Usage:
#   tools/package_release.sh <build-dir> <package-name> <output-dir>
#
# Writes <output-dir>/<package-name>.tar.gz with a single <package-name>/ folder inside.
set -euo pipefail

build_dir="$(realpath "$1")"
name="$2"
out_dir="$(realpath "$3")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cmake --install "$build_dir" --prefix "$work/install" >/dev/null

pkg="$work/$name"
mkdir -p "$pkg/lib"
cd "$work/install"
cp -a linux-wallpaperengine liblinux-wallpaperengine-lib.so libcef.so libEGL.so libGLESv2.so \
    libvk_swiftshader.so vk_swiftshader_icd.json chrome-sandbox icudtl.dat v8_context_snapshot.bin \
    ./*.pak locales "$pkg/"
cp -a lib/libkissfft-float.so* "$pkg/lib/"

# CEF ships libcef.so with debug symbols, 1.4 GB instead of about 220 MB
strip --strip-unneeded "$pkg/linux-wallpaperengine" "$pkg/liblinux-wallpaperengine-lib.so" "$pkg/libcef.so"

mkdir -p "$out_dir"
tar -czf "$out_dir/$name.tar.gz" -C "$work" "$name"
echo "Wrote $out_dir/$name.tar.gz"
