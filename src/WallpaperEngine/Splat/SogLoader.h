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

// WebP to tightly packed RGBA8 through the FFmpeg we already link, no libwebp
DecodedImage decodeWebP (const std::string& bytes);

// a decoded PlayCanvas SOG splat cloud: three RGBA32F textures, one texel per splat in the source images' order;
// positions in the photo's camera frame (x right, y down, z forward)
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

// meta is meta.json, readFile fetches the images it names; SOG v2 only, throws std::runtime_error on bad input
SplatCloud loadSog (const std::string& meta, const FileReader& readFile);
} // namespace WallpaperEngine::Splat
