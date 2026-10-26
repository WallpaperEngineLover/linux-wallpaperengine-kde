#!/usr/bin/env bash
# The GitHub workflow's clang-build job (cmake.yml) on this machine: everything built with clang/clang++ in Debug and
# Release. The job runs on Ubuntu 24.04 with clang 18, whose -Werror warnings differ from newer clang's (a private field
# that is never read broke the job on 7bf52ed6 and passed with clang 22 here), so by default it builds inside an Ubuntu
# 24.04 rootfs with the job's own apt packages, under proot like the portable release build. Run it with the other build
# variants (DISABLE_KDE_FEATURES) of a release prep, before the smoke tests and the VM matrix.
#
# Usage:
#   tools/clang_build_check.sh --setup              make the rootfs (a copy of distro-rootfs/ubuntu-24.04 with clang and the
#                                                   packages of the clang-build job, taken from the workflow file), once
#   tools/clang_build_check.sh [--host] [Release] [Debug]    default: the rootfs, both types, Release first
#                                                   --host builds with the clang of this machine instead (build-clang-<type>)
# Build trees: build-ci-<type> (rootfs) or build-clang-<type> (host), CEF comes from the main build tree (hard links), so
# nothing is downloaded. Exit code 0 when every requested type built; the log of each is <tree>/build.log.
# PROOT points at the proot binary (default: proot on PATH, else ~/.local/share/arch-proot/proot).
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOTFS_DIR=${ROOTFS_DIR:-$HOME/.local/share/distro-rootfs}
CI_ROOTFS=$ROOTFS_DIR/ubuntu-24.04-ci
export PROOT=${PROOT:-$(command -v proot || echo "$HOME/.local/share/arch-proot/proot")}

if [ "${1:-}" = --setup ]; then
    [ -d "$ROOTFS_DIR/ubuntu-24.04" ] || { echo "no $ROOTFS_DIR/ubuntu-24.04, tools/distro_rootfs.sh fetch ubuntu:24.04 it first"; exit 1; }
    [ -d "$CI_ROOTFS" ] || cp -a "$ROOTFS_DIR/ubuntu-24.04" "$CI_ROOTFS"
    packages=$(awk '/^  clang-build:/ { in_job = 1 } in_job && /apt-get -y install/ { sub(/.*apt-get -y install /, ""); print; exit }' \
        "$ROOT/.github/workflows/cmake.yml")
    [ -n "$packages" ] || { echo "cannot find the clang-build job's packages in cmake.yml"; exit 1; }
    "$ROOT/tools/distro_rootfs.sh" run "$CI_ROOTFS" bash -c \
        "export DEBIAN_FRONTEND=noninteractive; apt-get update >/dev/null && apt-get -y install clang cmake make git pkg-config ccache ca-certificates $packages && clang++ --version | head -1"
    exit $?
fi

host=0
types=()
for argument in "$@"; do
    case $argument in
    --host) host=1 ;;
    *) types+=("$argument") ;;
    esac
done
[ ${#types[@]} -gt 0 ] || types=(Release Debug)
jobs=${JOBS:-$(nproc)}
status=0
if [ $host = 0 ] && [ ! -d "$CI_ROOTFS" ]; then
    echo "FAIL clang build: no $CI_ROOTFS, run tools/clang_build_check.sh --setup first (or --host)"; exit 1
fi

for type in "${types[@]}"; do
    if [ $host = 1 ]; then dir=$ROOT/build-clang-$type; else dir=$ROOT/build-ci-$type; fi
    mkdir -p "$dir"
    if [ ! -e "$dir/cef" ] && [ -d "$ROOT/build/cef" ]; then
        cp -al "$ROOT/build/cef" "$dir/cef" 2>/dev/null || cp -a "$ROOT/build/cef" "$dir/cef"
    fi
    if [ $host = 1 ]; then
        label="clang $type ($(clang++ --version | head -1))"
        cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE="$type" -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++ \
            > "$dir/configure.log" 2>&1 && cmake --build "$dir" --config "$type" -j "$jobs" > "$dir/build.log" 2>&1
    else
        label="clang $type (Ubuntu 24.04 rootfs, clang 18)"
        BINDS="-b $ROOT:/src -b $dir:/build" "$ROOT/tools/distro_rootfs.sh" run "$CI_ROOTFS" bash -c "
            cmake -S /src -B /build -DCMAKE_BUILD_TYPE=$type -D CMAKE_C_COMPILER=clang -D CMAKE_CXX_COMPILER=clang++ \
                > /build/configure.log 2>&1 && cmake --build /build --config $type -j $jobs > /build/build.log 2>&1"
    fi
    if [ $? -eq 0 ]; then
        echo "PASS $label: built"
    else
        echo "FAIL $label: see $dir/configure.log and $dir/build.log"
        grep -m5 'error:' "$dir/build.log" 2>/dev/null | cut -c1-240
        status=1
    fi
done
exit $status
