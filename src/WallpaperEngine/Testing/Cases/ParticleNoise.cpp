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
    float biggestStep = 0.0f;
    float previous = simplexNoise3D (-50.0f, 3.1f - 50.0f * 0.37f, 7.7f + 50.0f * 0.21f);

    for (int i = 0; i < 20000; i++) {
	const float t = -50.0f + static_cast<float> (i) * 0.005f;
	const float one = simplexNoise1D (t);
	const float three = simplexNoise3D (t, 3.1f + t * 0.37f, 7.7f - t * 0.21f);

	min1 = std::min (min1, one);
	max1 = std::max (max1, one);
	min3 = std::min (min3, three);
	max3 = std::max (max3, three);
	biggestStep = std::max (biggestStep, std::abs (three - previous));
	previous = three;
    }

    CHECK (min1 >= -1.1f);
    CHECK (max1 <= 1.1f);
    CHECK (min3 >= -1.1f);
    CHECK (max3 <= 1.1f);
    CHECK (max3 - min3 > 1.0f);
    CHECK (biggestStep < 0.1f);
}
