#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace WallpaperEngine::Media {
struct ThumbnailPalette {
    glm::vec3 primary = { 0.12f, 0.12f, 0.12f };
    glm::vec3 secondary = { 0.0f, 0.0f, 0.0f };
    glm::vec3 tertiary = { 0.25f, 0.25f, 0.25f };
    glm::vec3 text = { 1.0f, 1.0f, 1.0f };
    glm::vec3 highContrast = { 1.0f, 1.0f, 1.0f };
};

ThumbnailPalette computeThumbnailPalette (const uint8_t* rgba, size_t width, size_t height);

ThumbnailPalette loadThumbnailPalette (const std::string& path);
}
