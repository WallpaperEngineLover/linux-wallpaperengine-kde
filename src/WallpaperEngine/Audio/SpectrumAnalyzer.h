#pragma once

#include <cstddef>
#include <vector>

#include "kiss_fft.h"

namespace WallpaperEngine::Audio {
/**
 * Turns captured samples into the 64 band left/right spectrum the way wallpaper64.exe's capture thread does
 * (sub_1400B2850). Samples are collected into blocks of two thirds of the FFT size, every block is transformed
 * on its own (no overlap) and the bins are folded into 64 bands on a pow(x, 0.25) curve. The result is linear
 * magnitude, SpectrumProcessor turns it into what shaders and scripts see.
 */
class SpectrumAnalyzer {
public:
    static constexpr int BANDS = 64;

    explicit SpectrumAnalyzer (int sampleRate);
    ~SpectrumAnalyzer ();

    SpectrumAnalyzer (const SpectrumAnalyzer&) = delete;
    SpectrumAnalyzer& operator= (const SpectrumAnalyzer&) = delete;

    /**
     * Feeds one packet of interleaved samples (1 or 2 channels, -1..1). WE takes a packet only up to the end of
     * the block it is filling and drops the rest of it, so does this.
     *
     * @param bands Receives [left 64 | right 64] when a block completed
     * @return If a block completed and bands was written
     */
    bool feed (const float* samples, std::size_t frames, int channels, float* bands);

    /** Drops the partially filled block, WE does this on silent packets and capture errors */
    void reset ();

    [[nodiscard]] int getFFTSize () const { return this->m_size; }
    [[nodiscard]] int getBlockSize () const { return this->m_blockSize; }

private:
    void analyze (int channels, float* bands);

    int m_size;
    int m_blockSize;
    int m_binCount;
    int m_filled = 0;
    kiss_fft_cfg m_config;
    std::vector<kiss_fft_cpx> m_input[2];
    std::vector<kiss_fft_cpx> m_output;
};
} // namespace WallpaperEngine::Audio
