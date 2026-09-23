#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <glm/vec3.hpp>
#include <glm/geometric.hpp>

namespace WallpaperEngine::Media {
/**
 * The colors scripts receive with a media thumbnail event.
 */
struct ThumbnailPalette {
    glm::vec3 primary = { 0.12f, 0.12f, 0.12f };
    glm::vec3 secondary = { 0.0f, 0.0f, 0.0f };
    glm::vec3 tertiary = { 0.25f, 0.25f, 0.25f };
    /** legible on top of the primary color */
    glm::vec3 text = { 1.0f, 1.0f, 1.0f };
    /** black or white, whichever stands out most against the primary color */
    glm::vec3 highContrast = { 1.0f, 1.0f, 1.0f };
};

/**
 * Picks the dominant colors of an RGBA8 image: the most common colors that are visibly different
 * from each other, most common first.
 */
ThumbnailPalette computeThumbnailPalette (const uint8_t* rgba, size_t width, size_t height);

/**
 * Decodes the image at the given path and returns its palette, or the default one if it cannot be read.
 */
ThumbnailPalette loadThumbnailPalette (const std::string& path);
} // namespace WallpaperEngine::Media
