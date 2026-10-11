#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace WallpaperEngine::Media {
struct CoverImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

/** Path of a percent encoded file:// MPRIS art URL, nothing for other schemes or bad escapes */
std::optional<std::filesystem::path> localCoverPath (std::string_view url);

/** Format is sniffed, browsers write WebP into .jpg files. Refuses sides over 4096 */
std::optional<CoverImage> decodeCoverImage (std::span<const uint8_t> bytes);

std::optional<CoverImage> loadCoverImage (const std::filesystem::path& path);

constexpr size_t MAX_COVER_BYTES = 16 * 1024 * 1024;
} // namespace WallpaperEngine::Media
