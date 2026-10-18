#pragma once

#include <cstddef>
#include <vector>

#include "kiss_fft.h"

namespace WallpaperEngine::Audio {
// 64 band left/right spectrum like wallpaper64.exe's capture thread (sub_1400B2850): blocks of 2/3 the FFT size, no
// overlap, bins folded on a pow(x, 0.25) curve, linear magnitude
class SpectrumAnalyzer {
public:
    static constexpr int BANDS = 64;

    explicit SpectrumAnalyzer (int sampleRate);
    ~SpectrumAnalyzer ();

    SpectrumAnalyzer (const SpectrumAnalyzer&) = delete;
    SpectrumAnalyzer& operator= (const SpectrumAnalyzer&) = delete;

    // one packet of 1-2 channel samples, the rest past the block's end is dropped like WE; true with bands [left 64 |
    // right 64] when a block completed
    bool feed (const float* samples, std::size_t frames, int channels, float* bands);

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
}
