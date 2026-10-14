#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace WallpaperEngine::Media {
// the colors scripts get with a media thumbnail event
struct ThumbnailPalette {
    glm::vec3 primary = { 0.12f, 0.12f, 0.12f };
    glm::vec3 secondary = { 0.0f, 0.0f, 0.0f };
    glm::vec3 tertiary = { 0.25f, 0.25f, 0.25f };
    /** legible on top of the primary color */
    glm::vec3 text = { 1.0f, 1.0f, 1.0f };
    /** black or white, whichever stands out most against the primary color */
    glm::vec3 highContrast = { 1.0f, 1.0f, 1.0f };
};

// the most common, visibly different colors of an RGBA8 image, most common first
ThumbnailPalette computeThumbnailPalette (const uint8_t* rgba, size_t width, size_t height);

// the default palette if the image can't be read
ThumbnailPalette loadThumbnailPalette (const std::string& path);
} // namespace WallpaperEngine::Media
