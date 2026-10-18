#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "WallpaperEngine/Input/ForwardedButton.h"

using WallpaperEngine::Input::forwardedLeftDown;
using namespace std::chrono_literals;

namespace {
std::filesystem::path write (const std::string& name, const std::string& content) {
    const auto path = std::filesystem::temp_directory_path () / name;
    std::ofstream (path) << content;
    return path;
}
}

TEST_CASE ("The button a desktop shell forwards is read from its file") {
    const auto path = write ("lwe-forwarded-button-test", R"({"leftDown": true})");

    CHECK (forwardedLeftDown (path));

    write ("lwe-forwarded-button-test", R"({"leftDown": false})");
    CHECK_FALSE (forwardedLeftDown (path));

    write ("lwe-forwarded-button-test", R"({"somethingElse": true})");
    CHECK_FALSE (forwardedLeftDown (path));

    std::filesystem::remove (path);
}

TEST_CASE ("A forwarded button counts as released when nothing says otherwise") {
    const auto path = write ("lwe-forwarded-button-stale", R"({"leftDown": true})");
    const auto written = std::filesystem::last_write_time (path);

    CHECK (forwardedLeftDown (path, 2s, written + 1s));
    CHECK_FALSE (forwardedLeftDown (path, 2s, written + 3s));

    write ("lwe-forwarded-button-stale", R"({"leftDown": tr)");
    CHECK_FALSE (forwardedLeftDown (path));

    write ("lwe-forwarded-button-stale", "[true]");
    CHECK_FALSE (forwardedLeftDown (path));

    std::filesystem::remove (path);

    CHECK_FALSE (forwardedLeftDown (path));
    CHECK_FALSE (forwardedLeftDown ({}));
}
