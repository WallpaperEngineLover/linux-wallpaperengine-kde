#include "SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace WallpaperEngine::Audio;

namespace {
// wallpaper64.exe's capture thread settings (processor constructor sub_1400A9130)
constexpr float BAND_EXPONENT = 0.25f;
constexpr float TILT = 0.5f;
constexpr float SIZE_FACTOR = 30.0f;
constexpr float BIN_FACTOR = 10.0f;
// "audioinputvolume" (default 50) * 0.02
constexpr float GAIN = 1.0f;
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer (int sampleRate) {
    const float rateScale = std::max (static_cast<float> (sampleRate) / 44100.0f, 1.0f);

    this->m_size = static_cast<int> (rateScale * 64.0f * SIZE_FACTOR);
    this->m_binCount = static_cast<int> (BIN_FACTOR * 64.0f);
    this->m_blockSize = static_cast<int> (
	static_cast<float> (this->m_size)
	- (BIN_FACTOR / SIZE_FACTOR) * static_cast<float> (this->m_size)
    );
    this->m_config = kiss_fft_alloc (this->m_size, 0, nullptr, nullptr);

    // samples are stored as s * 127 + 127 with 1 / that in the imaginary part, the part of the buffer past the
    // block is never written and keeps the silence value
    for (auto& input : this->m_input) {
	input.assign (this->m_size, kiss_fft_cpx { 127.0f, 1.0f / 127.0f });
    }

    this->m_output.resize (this->m_size);
}

SpectrumAnalyzer::~SpectrumAnalyzer () { kiss_fft_free (this->m_config); }

void SpectrumAnalyzer::reset () { this->m_filled = 0; }

bool SpectrumAnalyzer::feed (const float* samples, std::size_t frames, int channels, float* bands) {
    channels = std::clamp (channels, 1, 2);

    const int end = static_cast<int> (std::min<std::size_t> (this->m_blockSize, this->m_filled + frames));

    for (int i = this->m_filled; i < end; i++) {
	const float* frame = samples + static_cast<std::size_t> (i - this->m_filled) * channels;

	for (int c = 0; c < channels; c++) {
	    const float value = frame[c] * 127.0f + 127.0f;

	    this->m_input[c][i] = { value, 1.0f / value };
	}
    }

    this->m_filled = end;

    if (this->m_filled < this->m_blockSize) {
	return false;
    }

    this->m_filled = 0;
    this->analyze (channels, bands);

    return true;
}

void SpectrumAnalyzer::analyze (int channels, float* bands) {
    std::fill_n (bands, BANDS * 2, 0.0f);

    const float lastBin = static_cast<float> (this->m_binCount - 1);

    for (int c = 0; c < channels; c++) {
	kiss_fft (this->m_config, this->m_input[c].data (), this->m_output.data ());

	float* out = bands + c * BANDS;
	int band = 0;

	for (int bin = 1; bin < this->m_binCount; bin++) {
	    const kiss_fft_cpx& value = this->m_output[bin];
	    float power = value.r * value.r + value.i * value.i;

	    if (!std::isfinite (power)) {
		power = 0.0f;
	    }

	    const float x = static_cast<float> (bin - 1);
	    const float weight = TILT - std::cos (x * 3.1415927f / lastBin) * (1.0f - TILT);
	    const float magnitude = std::sqrt (weight * power);
	    const int target = static_cast<int> (std::pow (x / lastBin, BAND_EXPONENT) * 64.0f) % 64;

	    // the low bins would all land in the first few bands, each takes the next band until the curve catches up
	    band = std::min (band + 1, target);
	    out[band] = std::max (out[band], magnitude);
	}
    }

    if (channels < 2) {
	std::copy_n (bands, BANDS, bands + BANDS);
    }

    const float scale = GAIN * 0.001f
	* (static_cast<float> (this->m_binCount) / (static_cast<float> (this->m_size) * 0.5f));

    for (int i = 0; i < BANDS * 2; i++) {
	bands[i] *= scale;
    }
}
