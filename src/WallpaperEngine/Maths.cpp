#include "Maths.h"

#include <cmath>

using namespace WallpaperEngine::Maths;

float WallpaperEngine::Maths::randomFloat (std::mt19937& rng, float min, float max) {
    if (max < min) {
	std::swap (min, max);
    }
    std::uniform_real_distribution<float> dist (min, max);
    return dist (rng);
}

glm::vec3 WallpaperEngine::Maths::randomVec3 (std::mt19937& rng, const glm::vec3& min, const glm::vec3& max) {
    return glm::vec3 (
	randomFloat (rng, min.x, max.x), randomFloat (rng, min.y, max.y), randomFloat (rng, min.z, max.z)
    );
}

// Helper: Linear interpolation
float WallpaperEngine::Maths::lerp (float t, float a, float b) { return a + t * (b - a); }

// Helper: Fade value change over lifetime
float WallpaperEngine::Maths::fadeValue (float life, float startTime, float endTime, float startValue, float endValue) {
    if (life <= startTime) {
	return startValue;
    } else if (life >= endTime) {
	return endValue;
    } else {
	float t = (life - startTime) / (endTime - startTime);
	return lerp (t, startValue, endValue);
    }
}

/** sub_1401B8C70 */
glm::vec3 WallpaperEngine::Maths::hsvToRgb (const glm::vec3& hsv) {
    const float chroma = hsv.z * hsv.y;
    const float sector = static_cast<float> (std::fmod (static_cast<double> (hsv.x * 6.0f), 6.0));
    const float second = static_cast<float> (
	(1.0 - std::fabs (std::fmod (static_cast<double> (sector), 2.0) - 1.0)) * static_cast<double> (chroma)
    );
    const float base = hsv.z - chroma;

    glm::vec3 rgb (0.0f);
    if (sector >= 0.0f && sector < 1.0f) {
	rgb = { chroma, second, 0.0f };
    } else if (sector >= 1.0f && sector < 2.0f) {
	rgb = { second, chroma, 0.0f };
    } else if (sector >= 2.0f && sector < 3.0f) {
	rgb = { 0.0f, chroma, second };
    } else if (sector >= 3.0f && sector < 4.0f) {
	rgb = { 0.0f, second, chroma };
    } else if (sector >= 4.0f && sector < 5.0f) {
	rgb = { second, 0.0f, chroma };
    } else if (sector >= 5.0f && sector < 6.0f) {
	rgb = { chroma, 0.0f, second };
    }
    return rgb + base;
}

/** sub_1401B8B90 */
glm::vec3 WallpaperEngine::Maths::rgbToHsv (const glm::vec3& rgb) {
    const float max = std::fmax (std::fmax (rgb.r, rgb.g), rgb.b);
    const float min = std::fmin (std::fmin (rgb.r, rgb.g), rgb.b);
    const float range = max - min;
    glm::vec3 hsv (0.0f, 0.0f, max);

    if (range < 0.0000099999997f || max <= 0.0f) {
	return hsv;
    }

    hsv.y = range / max;
    float hue;
    if (rgb.r < max) {
	hue = rgb.g < max ? (rgb.r - rgb.g) / range + 4.0f : (rgb.b - rgb.r) / range + 2.0f;
    } else {
	hue = (rgb.g - rgb.b) / range;
    }
    hsv.x = hue / 6.0f;
    if (hsv.x < 0.0f) {
	hsv.x += 1.0f;
    }
    return hsv;
}
