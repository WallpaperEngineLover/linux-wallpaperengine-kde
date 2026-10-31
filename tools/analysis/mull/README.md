Mutation testing of the files with unit tests (Hrtf, SpectrumProcessor, SpectrumAnalyzer, SplitLayout, ThumbnailPalette,
LedLayout, AudioStream, ForwardedButton). Mull 0.35.0 for LLVM 22 is unpacked in `~/.local/share/mull-dl/x`
(`MULL_PLUGIN` overrides the plugin path).

    cmake -S . -B build-mull -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=1 -DCMAKE_CXX_FLAGS=-Wno-error \
        -DCMAKE_CXX_COMPILER=$PWD/tools/analysis/mull/mull-clangxx
    make -C build-mull -j8 tests
    export MULL_CONFIG=$PWD/tools/analysis/mull/mull.yml
    ~/.local/share/mull-dl/x/usr/bin/mull-runner-22 build-mull/output/tests --workers 8 --timeout 600000 \
        --minimum-timeout 30000 --ld-search-path /usr/lib64 --reporters IDE --report-dir <dir>

The wrapper adds the plugin only for the files it lists (the plugin on everything crashes in the junk detector on
External sources), so a new file under test goes into both the wrapper's `case` and `includePaths` in `mull.yml`, and an
object compiled before that has to be touched to be rebuilt. Test names must not contain commas when a Catch2 filter is
passed after `--`. A full pass takes about 15 minutes.
