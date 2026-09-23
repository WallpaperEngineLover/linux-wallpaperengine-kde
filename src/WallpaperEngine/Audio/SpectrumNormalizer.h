#pragma once

#include <cstddef>

namespace WallpaperEngine::Audio {
/**
 * Turns the recorder's unbounded log scaled band levels into 0-1 values that keep the shape of the spectrum at any
 * volume. The top of the range follows the loudest fifth of the bands (rising quickly, falling back over a few
 * seconds), and levels below a gate are never shown so silence and capture noise stay flat.
 */
class SpectrumNormalizer {
public:
    /** Feeds the band levels of a frame (at most 64), dt being the time since the previous one */
    void update (const float* levels, std::size_t count, float dt);

    /** Maps one band level of the frame last given to update() into 0-1 */
    [[nodiscard]] float apply (float level) const;

    [[nodiscard]] float getReference () const { return this->m_reference; }

private:
    float m_reference = 0.0f;
};
} // namespace WallpaperEngine::Audio
