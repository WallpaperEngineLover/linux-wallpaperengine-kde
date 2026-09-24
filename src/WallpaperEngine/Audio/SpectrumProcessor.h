#pragma once

namespace WallpaperEngine::Audio {
/**
 * The per-frame half of wallpaper64.exe's audio pipeline (main loop sub_1400E7240): scales each group of 8 bands
 * by a slowly tracking level of that group, smooths the result and limits how fast it may move, then builds the
 * 64/32/16 band buffers shaders, scripts and particles read. Each buffer is [left | right | average].
 */
class SpectrumProcessor {
public:
    static constexpr int GROUPS = 16;

    /**
     * @param raw [left 64 | right 64] from SpectrumAnalyzer
     * @param dt Seconds since the previous frame (already scaled by playback speed)
     */
    void update (const float* raw, float dt);

    float audio16[16 * 3] = { 0 };
    float audio32[32 * 3] = { 0 };
    float audio64[64 * 3] = { 0 };

    [[nodiscard]] const float* getGroupLevels () const { return this->m_level; }

private:
    float m_level[GROUPS] = { 0 };
    float m_smoothed[128] = { 0 };
    float m_output[128] = { 0 };
};
} // namespace WallpaperEngine::Audio
