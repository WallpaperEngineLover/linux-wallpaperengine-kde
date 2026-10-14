#pragma once

namespace WallpaperEngine::Audio {
// per-frame half of WE's audio pipeline (sub_1400E7240): per-group level tracking, smoothing and rate limits, then the
// [left | right | average] 64/32/16 band buffers
class SpectrumProcessor {
public:
    static constexpr int GROUPS = 16;

    // raw is [left 64 | right 64] from SpectrumAnalyzer, dt already scaled by playback speed
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
