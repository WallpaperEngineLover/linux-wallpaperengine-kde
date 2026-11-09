#!/usr/bin/env bash
# The coverage of what only runs in a VM: Gala (PantheonShell, the XWayland fallback), the Flatpak sandbox (HostWayland),
# real DPMS and the other compositors of the VM matrix. The VMs run a coverage build of the portable release, made in the
# Debian 12 rootfs under proot (GCC 12, glibc 2.35 like the release) whose launcher exports GCOV_PREFIX, so the profile data
# lands in /tmp/lwe-gcov of the guest; release_check.sh copies it out (COLLECT_GCOV). The host's gcov (newer GCC) cannot read
# that data and gcovr drops some of those files: tools/analysis/gcov12_report.py runs the rootfs's own gcov and reads its text output. A report of the VM runs is merged with the
# host's (tools/analysis/merge_coverage.py): only lines the host build knows count, GCC 12 splits constructors and inlined
# code differently from GCC 16, so a file's gain in a file that also runs on the host is not a VM-only gain.
# Usage: tools/analysis/coverage_vms.sh <stage> [out dir]
#   build    configure and build in the rootfs (build dir ~/.local/share/distro-rootfs/build-deb12-cov), package, candidate
#   run      release_check.sh lanes (VMS="elementary debian-xfce fedora-kde arch-hypr"), arch-hypr also runs the Flatpak step
#            with FLATPAK_BUNDLE (a we_manager .flatpak)
#   report   line counts of the collected data, merged with the host's report (build-cov must be a finished coverage run)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
stage=${1:?build|run|report}
OUT=${2:-$HOME/.cache/lwe-analysis/coverage-vms}
B=$HOME/.local/share/distro-rootfs/build-deb12-cov
ROOTFS=$HOME/.local/share/distro-rootfs/debian-12
export PROOT=${PROOT:-$HOME/.local/share/arch-proot/proot}
mkdir -p "$OUT"

case $stage in
build)
    mkdir -p "$B/out"
    [ -d "$B/cef" ] || cp -a "$HOME/.local/share/distro-rootfs/build-deb12/cef" "$B/cef"
    cat > "$B/build.sh" <<'EOS'
#!/bin/bash
set -e
cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DDISABLE_KDE_FEATURES=OFF -DBUILD_TESTING=0 \
    "-DCMAKE_CXX_FLAGS=--coverage -U_FORTIFY_SOURCE -ffile-prefix-map=/src=/workspace/linux-wallpaperengine-kde" \
    "-DCMAKE_C_FLAGS=--coverage -ffile-prefix-map=/src=/workspace/linux-wallpaperengine-kde" \
    -DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage > /build/configure.log 2>&1
cmake --build /build --parallel 14 > /build/build.log 2>&1
cd /src && tools/package_release.sh --portable /build lwe-cov /build/out
EOS
    chmod +x "$B/build.sh"
    BINDS="-b $HERE:/src -b $B:/build" "$HERE/tools/distro_rootfs.sh" run "$ROOTFS" /build/build.sh || exit 1
    rm -rf "$B/candidate"
    mkdir -p "$B/candidate"
    tar -C "$B/candidate" -xzf "$B/out/lwe-cov.tar.gz"
    mv "$B/candidate/lwe-cov" "$B/candidate/lwe-hints"
    sed -i 's|^here=.*|&\nexport GCOV_PREFIX=/tmp/lwe-gcov GCOV_PREFIX_STRIP=0|' "$B/candidate/lwe-hints/linux-wallpaperengine"
    ;;
run)
    for vm in ${VMS:-elementary debian-xfce fedora-kde arch-hypr}; do
        step=""
        [ "$vm" = arch-hypr ] && [ -n "${FLATPAK_BUNDLE:-}" ] && step="$HERE/tools/linuxvm/coverage_flatpak.sh"
        COLLECT_GCOV="$OUT/gcda" COVERAGE_STEP=$step FLATPAK_BUNDLE=${FLATPAK_BUNDLE:-} REPORT_DIR="$OUT/report-$vm" \
            "$HERE/tools/linuxvm/release_check.sh" --vm "$vm" --candidate "$B/candidate"
    done
    ;;
report)
    cd "$HERE" || exit 1
    args=()
    for dir in "$OUT"/gcda/*/; do
        name=$(basename "$dir")
        rm -rf "$OUT/cov-$name"
        mkdir -p "$OUT/cov-$name"
        rsync -a --include='*/' --include='*.gcno' --exclude='*' "$B/CMakeFiles" "$B/spirv-cross" "$OUT/cov-$name/"
        cp -a "$dir/build/." "$OUT/cov-$name/"
        python3 "$HERE/tools/analysis/gcov12_report.py" "$OUT/cov-$name" "$OUT/vm-$name.json" > /dev/null
        args+=("$OUT/vm-$name.json")
    done
    gcovr -r . --object-directory build-cov --gcov-ignore-parse-errors suspicious_hits.warn_once_per_file \
        --filter 'src/WallpaperEngine/' --json "$OUT/host.json" > /dev/null 2>&1
    python3 "$HERE/tools/analysis/merge_coverage.py" "$OUT/host.json" "${args[@]}"
    ;;
esac
