#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "WallpaperEngine/WebBrowser/CEF/TestHookPolicy.h"

using namespace WallpaperEngine::WebBrowser::CEF;

TEST_CASE ("Debug switches cannot turn the web sandbox off") {
    CHECK (weakensWebSandbox ("--no-sandbox"));
    CHECK (weakensWebSandbox ("no-sandbox"));
    CHECK (weakensWebSandbox ("--remote-debugging-port=9222"));
    CHECK (weakensWebSandbox ("--renderer-cmd-prefix=/bin/sh -c"));
    CHECK (weakensWebSandbox ("--js-flags=--allow-natives-syntax"));
    CHECK (weakensWebSandbox ("--disable-gpu-sandbox"));

    CHECK_FALSE (weakensWebSandbox ("--ozone-platform=headless"));
    CHECK_FALSE (weakensWebSandbox ("--use-angle=gl-egl"));
    CHECK_FALSE (weakensWebSandbox ("--disable-dev-shm-usage"));
    CHECK_FALSE (weakensWebSandbox ("--ignore-gpu-blocklist"));
    CHECK_FALSE (weakensWebSandbox ("--no-sandbox-but-not-really-one-of-them"));
}

TEST_CASE ("A preload library has to be ours and not writable by others") {
    const auto directory = std::filesystem::temp_directory_path () / ("lwe-preload-test-" + std::to_string (getpid ()));
    std::filesystem::create_directories (directory);
    const auto library = directory / "libcef.so";
    std::ofstream (library) << "x";

    chmod (library.c_str (), 0644);
    CHECK (trustedPreloadFile (library.string ()));

    chmod (library.c_str (), 0664);
    CHECK_FALSE (trustedPreloadFile (library.string ()));

    chmod (library.c_str (), 0646);
    CHECK_FALSE (trustedPreloadFile (library.string ()));

    chmod (library.c_str (), 0644);
    CHECK_FALSE (trustedPreloadFile ("libcef.so"));
    CHECK_FALSE (trustedPreloadFile ((directory / "missing.so").string ()));
    CHECK_FALSE (trustedPreloadFile (directory.string ()));

    std::filesystem::remove_all (directory);
}
