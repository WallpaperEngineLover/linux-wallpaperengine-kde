#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace WallpaperEngine::Splat {
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// Decodes a WebP file already loaded in memory into tightly packed RGBA8. Goes through the FFmpeg
// the engine already links for audio, instead of pulling in libwebp as a separate dependency.
DecodedImage decodeWebP (const std::string& bytes);

// A decoded SOG (PlayCanvas "spatially ordered gaussians") splat cloud, ready to be uploaded as
// three RGBA32F textures of textureWidth x textureHeight texels, one texel per splat, in the same
// row-major order the source images use. Positions are in the source photo's own camera frame
// (x right, y down, z forward), untouched by any viewer transform.
struct SplatCloud {
    uint32_t count = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;

    // xyz = center, w = the splat's RGBA8 color (r in the low byte) stored as a raw uint32 bit pattern
    std::vector<float> centerAndColor;
    // xyzw = unit rotation quaternion
    std::vector<float> rotation;
    // xyz = linear (already exponentiated) per-axis scale, w unused
    std::vector<float> scale;
};

using FileReader = std::function<std::string (const std::string& name)>;

// meta is the SOG meta.json contents; readFile fetches the image files it references by name.
// Only SOG version 2 (codebook quantized) is supported. Throws std::runtime_error on bad input.
SplatCloud loadSog (const std::string& meta, const FileReader& readFile);
} // namespace WallpaperEngine::Splat
