#pragma once

#include <glm/vec3.hpp>
#include <random>

namespace WallpaperEngine::Maths {
float randomFloat (std::mt19937& rng, float min, float max);
glm::vec3 randomVec3 (std::mt19937& rng, const glm::vec3& min, const glm::vec3& max);
float lerp (float t, float a, float b);
float fadeValue (float life, float startTime, float endTime, float startValue, float endValue);
/** wallpaper64.exe's HSV conversions, all channels 0..1 */
glm::vec3 hsvToRgb (const glm::vec3& hsv);
glm::vec3 rgbToHsv (const glm::vec3& rgb);
}