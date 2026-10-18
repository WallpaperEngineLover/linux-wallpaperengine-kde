#pragma once

#include <chrono>
#include <filesystem>

namespace WallpaperEngine::Input {
/**
 * The left button a desktop shell forwards through --input-file ({"leftDown": true}, rewritten while held). A file
 * older than maxAge, missing or half written counts as released.
 */
bool forwardedLeftDown (
    const std::filesystem::path& path, std::chrono::seconds maxAge = std::chrono::seconds (2),
    std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now ()
);
}
