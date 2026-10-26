#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "OpenRgbProtocol.h"

namespace WallpaperEngine::Led {
constexpr int kFrameWidth = 80;
constexpr int kFrameHeight = 10;
constexpr size_t kFrameBytes = static_cast<size_t> (kFrameWidth) * kFrameHeight * 3;

struct Brightness {
    bool boost = true;
    float factor = 1.0f;
};

struct LedPixel {
    int x = 0;
    int y = 0;
};

[[nodiscard]] std::vector<LedPixel> layoutDevice (const OpenRgb::Device& device);

/** ledextensions64.dll's frame callback (sub_180012448): the boost and the brightness factor, rgb in and out */
void applyBrightness (std::span<const uint8_t> in, std::span<uint8_t> out, const Brightness& brightness);

void sampleDevice (std::span<const uint8_t> frame, std::span<const LedPixel> layout, std::vector<uint8_t>& out);
}
