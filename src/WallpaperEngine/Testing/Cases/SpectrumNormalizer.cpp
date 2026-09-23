#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Audio/SpectrumNormalizer.h"

using Catch::Approx;
using WallpaperEngine::Audio::SpectrumNormalizer;

namespace {
// a frame whose loudest fifth of the bands sit at the given level, and the rest well below it
void settle (SpectrumNormalizer& normalizer, float peak, float seconds) {
    constexpr float frame = 0.023f;
    float levels[64] = {};

    for (int i = 0; i < 13; i++) {
	levels[i] = peak;
    }

    for (float t = 0.0f; t < seconds; t += frame) {
	normalizer.update (levels, 64, frame);
    }
}
} // namespace

TEST_CASE ("Silence and capture noise stay flat") {
    SpectrumNormalizer normalizer;

    settle (normalizer, 0.0f, 2.0f);

    CHECK (normalizer.apply (0.0f) == 0.0f);
    CHECK (normalizer.apply (0.1f) == 0.0f);
}

TEST_CASE ("Loud music does not push every band to the top") {
    SpectrumNormalizer normalizer;

    settle (normalizer, 2.5f, 3.0f);

    CHECK (normalizer.apply (2.5f) == Approx (1.0f).margin (0.02f));
    // a band a little below the peak is clearly below the top, not locked to it
    CHECK (normalizer.apply (2.1f) < 0.7f);
    CHECK (normalizer.apply (2.1f) > 0.2f);
    CHECK (normalizer.apply (1.0f) == 0.0f);
}

TEST_CASE ("The same shape looks the same at any volume") {
    SpectrumNormalizer loud;
    SpectrumNormalizer louder;

    settle (loud, 2.0f, 3.0f);
    settle (louder, 3.0f, 3.0f);

    // 0.3 below the peak in both
    CHECK (loud.apply (1.7f) == Approx (louder.apply (2.7f)).margin (0.02f));
}

TEST_CASE ("Faint sounds are stretched to fill the range but not below the gate") {
    SpectrumNormalizer normalizer;

    settle (normalizer, 0.6f, 3.0f);

    CHECK (normalizer.apply (0.6f) == Approx (1.0f).margin (0.02f));
    CHECK (normalizer.apply (0.15f) == 0.0f);
}

TEST_CASE ("The reference follows the music back down over a few seconds") {
    SpectrumNormalizer normalizer;

    settle (normalizer, 3.0f, 2.0f);
    CHECK (normalizer.getReference () == Approx (3.0f).margin (0.05f));

    settle (normalizer, 1.0f, 0.5f);
    CHECK (normalizer.getReference () > 2.0f);

    settle (normalizer, 1.0f, 10.0f);
    CHECK (normalizer.getReference () < 1.2f);
}

TEST_CASE ("One outlier band does not flatten the rest of the spectrum") {
    SpectrumNormalizer normalizer;
    float levels[64];

    for (float& level : levels) {
	level = 1.5f;
    }

    levels[10] = 4.0f;

    for (int i = 0; i < 200; i++) {
	normalizer.update (levels, 64, 0.023f);
    }

    CHECK (normalizer.getReference () < 2.2f);
    CHECK (normalizer.apply (1.5f) > 0.5f);
}
