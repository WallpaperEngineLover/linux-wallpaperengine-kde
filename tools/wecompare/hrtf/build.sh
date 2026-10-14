#!/bin/bash
# build.sh [work dir]: downloads and builds OpenAL Soft 1.21.1 (the version inside WE's mediaextensions64.dll, with
# loopback rendering and its built-in HRTF only, no audio backends) and the harnesses ref, refwav and ours (ours links
# build/output/liblinux-wallpaperengine-lib.so, build the engine first). Default work dir ~/.cache/lwe-openal-1.21.1
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
W=${1:-$HOME/.cache/lwe-openal-1.21.1}
SHA=8ac17e4e3b32c1af3d5508acfffb838640669b4274606b7892aa796ca9d7467f
mkdir -p "$W"
cd "$W"
if [ ! -d openal-soft-1.21.1 ]; then
    curl -sSL -o openal-soft-1.21.1.tar.gz https://github.com/kcat/openal-soft/archive/refs/tags/1.21.1.tar.gz
    echo "$SHA  openal-soft-1.21.1.tar.gz" | sha256sum -c --quiet
    tar xzf openal-soft-1.21.1.tar.gz
fi
if [ ! -f build/libopenal.so.1.21.1 ]; then
    mkdir -p build
    # 1.21.1 predates CMake 4 and GCC 15's stricter template checks
    (cd build && CXXFLAGS="-Wno-template-body -include cstdint -include cstring" cmake ../openal-soft-1.21.1 \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
        -DALSOFT_TESTS=OFF -DALSOFT_NO_CONFIG_UTIL=ON -DALSOFT_EMBED_HRTF_DATA=ON \
        $(for b in PIPEWIRE PULSEAUDIO ALSA OSS JACK SDL2 PORTAUDIO SNDIO WAVE; do echo -DALSOFT_BACKEND_$b=OFF; done) \
        > cmake.log && make -j"$(nproc)" > make.log)
fi
INC=openal-soft-1.21.1/include
g++ -O2 -std=c++17 "$HERE/ref.cpp" -I$INC -Lbuild -lopenal -Wl,-rpath,"$W/build" -o ref
g++ -O2 -std=c++17 "$HERE/refwav.cpp" -I$INC -Lbuild -lopenal -Wl,-rpath,"$W/build" -o refwav
g++ -O2 -std=c++17 "$HERE/refstream.cpp" -I$INC -Lbuild -lopenal -Wl,-rpath,"$W/build" -o refstream
g++ -O2 -std=c++20 "$HERE/ours.cpp" -I"$REPO/src" -I"$REPO/build/generated" -L"$REPO/build/output" \
    -llinux-wallpaperengine-lib -Wl,-rpath,"$REPO/build/output" -o ours
echo "built ref, refwav, refstream and ours in $W"
