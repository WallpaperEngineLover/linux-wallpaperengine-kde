#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "WallpaperEngine/Render/Utils/NoiseUtils.h"

using WallpaperEngine::Render::Utils::simplexNoise1D;
using WallpaperEngine::Render::Utils::simplexNoise3D;

TEST_CASE ("Simplex noise is zero on lattice points") {
    CHECK (simplexNoise1D (0.0f) == 0.0f);
    CHECK (simplexNoise1D (7.0f) == 0.0f);
    CHECK (std::abs (simplexNoise3D (0.0f, 0.0f, 0.0f)) < 1e-6f);
}

TEST_CASE ("Simplex noise stays in range and is continuous") {
    float min1 = 0.0f, max1 = 0.0f, min3 = 0.0f, max3 = 0.0f;
    // only the 1D noise is continuous, WE's 3D hash (see simplexNoise3D) jumps at some cell borders
    float biggestStep = 0.0f;
    float previous = simplexNoise1D (-50.0f);

    for (int i = 0; i < 20000; i++) {
	const float t = -50.0f + static_cast<float> (i) * 0.005f;
	const float one = simplexNoise1D (t);
	const float three = simplexNoise3D (t, 3.1f + t * 0.37f, 7.7f - t * 0.21f);

	min1 = std::min (min1, one);
	max1 = std::max (max1, one);
	min3 = std::min (min3, three);
	max3 = std::max (max3, three);
	biggestStep = std::max (biggestStep, std::abs (one - previous));
	previous = one;
    }

    CHECK (min1 >= -1.1f);
    CHECK (max1 <= 1.1f);
    CHECK (min3 >= -1.1f);
    CHECK (max3 <= 1.1f);
    CHECK (max3 - min3 > 1.0f);
    CHECK (biggestStep < 0.1f);
}

TEST_CASE ("3D simplex noise matches wallpaper64.exe") {
    // sub_1400FD010 run under emulation with these inputs
    struct Sample {
	float x, y, z, expected;
    };
    const Sample samples[] = {
	{ -36.5635757f, 34.7433739f, 26.3774624f, 0.26951617f },
	{ -24.4930973f, -0.456491292f, -5.05089331f, 0.000486342702f },
	{ 26.2280083f, -49.7893944f, -5.46128082f, -0.404468179f },
	{ -28.3400612f, -7.78834248f, -47.0959206f, -0.373473674f },
	{ 33.7577972f, 5.64543247f, 14.2294359f, -0.48249346f },
    };

    for (const auto& sample : samples) {
	CHECK (std::abs (simplexNoise3D (sample.x, sample.y, sample.z) - sample.expected) < 1e-5f);
    }
}
