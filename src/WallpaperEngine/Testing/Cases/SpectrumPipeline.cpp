#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "WallpaperEngine/Audio/SpectrumAnalyzer.h"
#include "WallpaperEngine/Audio/SpectrumProcessor.h"

using Catch::Approx;
using WallpaperEngine::Audio::SpectrumAnalyzer;
using WallpaperEngine::Audio::SpectrumProcessor;

namespace {
std::vector<float> stereoTone (int frames, float frequency, int rate, float leftAmplitude, float rightAmplitude) {
    std::vector<float> samples (frames * 2);

    for (int i = 0; i < frames; i++) {
	const float value = std::sin (2.0f * 3.1415927f * frequency * static_cast<float> (i) / static_cast<float> (rate));

	samples[i * 2] = value * leftAmplitude;
	samples[i * 2 + 1] = value * rightAmplitude;
    }

    return samples;
}
} // namespace

TEST_CASE ("Spectrum analyzer uses WE's FFT and block sizes") {
    SpectrumAnalyzer at44 (44100);
    SpectrumAnalyzer at48 (48000);

    CHECK (at44.getFFTSize () == 1920);
    CHECK (at44.getBlockSize () == 1280);
    CHECK (at48.getFFTSize () == 2089);
    CHECK (at48.getBlockSize () == 1392);
}

TEST_CASE ("A low tone lands in the band of its bin, one bin per band at the bottom") {
    SpectrumAnalyzer analyzer (44100);
    float bands[128];
    // bin 10 of a 1920 point FFT at 44.1kHz, bins 1-30 get a band each (bin - 1)
    const auto samples = stereoTone (1280, 10.0f * 44100.0f / 1920.0f, 44100, 0.5f, 0.0f);

    REQUIRE (analyzer.feed (samples.data (), 1280, 2, bands));

    const auto loudest = std::max_element (bands, bands + 64) - bands;

    CHECK (loudest == 9);
    CHECK (bands[loudest] > 0.0f);
    CHECK (*std::max_element (bands + 64, bands + 128) < bands[loudest] * 0.001f);
}

TEST_CASE ("The packet completing a block is not carried into the next one") {
    SpectrumAnalyzer analyzer (44100);
    float bands[128];
    const auto samples = stereoTone (2000, 440.0f, 44100, 0.5f, 0.5f);

    CHECK (analyzer.feed (samples.data (), 2000, 2, bands));
    CHECK_FALSE (analyzer.feed (samples.data (), 1279, 2, bands));
    CHECK (analyzer.feed (samples.data (), 1, 2, bands));
}

TEST_CASE ("Mono input is copied to the right channel") {
    SpectrumAnalyzer analyzer (44100);
    float bands[128];
    std::vector<float> samples (1280);

    for (int i = 0; i < 1280; i++) {
	samples[i] = 0.5f * std::sin (2.0f * 3.1415927f * 1000.0f * static_cast<float> (i) / 44100.0f);
    }

    REQUIRE (analyzer.feed (samples.data (), 1280, 1, bands));

    for (int i = 0; i < 64; i++) {
	CHECK (bands[i] == bands[i + 64]);
    }
}

TEST_CASE ("Steady input settles at the level of its group") {
    SpectrumProcessor processor;
    float raw[128];

    std::fill_n (raw, 64, 0.5f);
    std::fill_n (raw + 64, 64, 0.25f);

    for (int frame = 0; frame < 600; frame++) {
	processor.update (raw, 1.0f / 60.0f);
    }

    CHECK (processor.getGroupLevels ()[0] == Approx (0.5f));
    CHECK (processor.getGroupLevels ()[8] == Approx (0.25f));
    CHECK (processor.audio64[0] == Approx (1.0f).margin (0.01));
    CHECK (processor.audio64[64] == Approx (1.0f).margin (0.01));
    CHECK (processor.audio64[128] == Approx (1.0f).margin (0.01));
}

TEST_CASE ("Buffers are left, right and average, lower resolutions take pairwise maxima") {
    SpectrumProcessor processor;
    float raw[128] = {};

    for (int i = 0; i < 128; i++) {
	raw[i] = 0.1f + 0.005f * static_cast<float> (i);
    }

    for (int frame = 0; frame < 120; frame++) {
	processor.update (raw, 1.0f / 60.0f);
    }

    for (int i = 0; i < 64; i++) {
	CHECK (processor.audio64[128 + i] == Approx ((processor.audio64[i] + processor.audio64[64 + i]) * 0.5f));
    }

    for (int i = 0; i < 96; i++) {
	CHECK (processor.audio32[i] == std::max (processor.audio64[i * 2], processor.audio64[i * 2 + 1]));
    }

    for (int i = 0; i < 48; i++) {
	CHECK (processor.audio16[i] == std::max (processor.audio32[i * 2], processor.audio32[i * 2 + 1]));
    }
}

TEST_CASE ("Silence drops the spectrum to zero at once") {
    SpectrumProcessor processor;
    float raw[128];
    const float silence[128] = {};

    std::fill_n (raw, 128, 0.5f);

    for (int frame = 0; frame < 120; frame++) {
	processor.update (raw, 1.0f / 60.0f);
    }

    REQUIRE (processor.audio16[0] > 0.5f);

    processor.update (silence, 1.0f / 60.0f);

    CHECK (processor.audio16[0] == 0.0f);
    CHECK (processor.audio64[127] == 0.0f);
}

TEST_CASE ("The first frame only moves part of the way") {
    SpectrumProcessor processor;
    float raw[128];

    std::fill_n (raw, 128, 0.5f);
    processor.update (raw, 1.0f / 60.0f);

    // the level starts at 1 and has moved down by dt / 2, then 20 * dt of the way to raw / level
    const float level = 1.0f - 0.5f / 60.0f;

    CHECK (processor.audio64[0] == Approx (0.5f / level * (20.0f / 60.0f)));
}
