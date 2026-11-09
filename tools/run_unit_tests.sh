#!/bin/bash
# Builds the Catch2 `tests` target and runs it with no display, a private runtime dir and no core dumps.
# usage: tools/run_unit_tests.sh [--build-dir <dir>] [--jobs <n>] [--no-build] [-- <Catch2 arguments>]
# Exit status is the test binary's (or the build's), so a release prep can gate on it.

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/build"
jobs=$(( $(nproc) > 4 ? 4 : $(nproc) ))
compile=1

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build=$(realpath -m "$2"); shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --no-build) compile=0; shift ;;
        --) shift; break ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ "$compile" = 1 ]; then
    if ! grep -qs '^BUILD_TESTING:.*=\(1\|ON\)$' "$build/CMakeCache.txt"; then
        cmake -S "$root" -B "$build" -DBUILD_TESTING=1 || exit 1
    fi

    nice cmake --build "$build" --target tests -j "$jobs" || { echo "unit tests: BUILD FAILED"; exit 1; }
fi

binary="$build/output/tests"

if [ ! -x "$binary" ]; then
    echo "unit tests: $binary does not exist" >&2
    exit 1
fi

runtime=$(mktemp -d)
trap 'rm -rf "$runtime"' EXIT
chmod 700 "$runtime"
ulimit -c 0

(cd "$build/output" && env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$runtime" timeout 1800 "$binary" "$@")
status=$?

if [ $status -eq 0 ]; then
    echo "unit tests: PASS"
else
    echo "unit tests: FAIL (exit $status)"
fi

exit $status
