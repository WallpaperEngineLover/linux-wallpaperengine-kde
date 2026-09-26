#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace WallpaperEngine::Render::Utils {

static const unsigned char PERLIN_PERM[]
    = { 151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37,
	240, 21, 10, 23, 190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177,
	33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166, 77, 146,
	158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54, 65, 25,
	63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100,
	109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206,
	59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153,
	101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246,
	97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192,
	214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114,
	67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180,
	// Duplicate for wrapping
	151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37,
	240, 21, 10, 23, 190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177,
	33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166, 77, 146,
	158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54, 65, 25,
	63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100,
	109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206,
	59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153,
	101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246,
	97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192,
	214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114,
	67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180 };

inline double perlinGrad (int hash, double x, double y, double z) {
    switch (hash & 0xF) {
	case 0x0:
	    return x + y;
	case 0x1:
	    return -x + y;
	case 0x2:
	    return x - y;
	case 0x3:
	    return -x - y;
	case 0x4:
	    return x + z;
	case 0x5:
	    return -x + z;
	case 0x6:
	    return x - z;
	case 0x7:
	    return -x - z;
	case 0x8:
	    return y + z;
	case 0x9:
	    return -y + z;
	case 0xA:
	    return y - z;
	case 0xB:
	    return -y - z;
	case 0xC:
	    return y + x;
	case 0xD:
	    return -y + z;
	case 0xE:
	    return y - x;
	case 0xF:
	    return -y - z;
	default:
	    return 0;
    }
}

// Perlin noise ease curve (6t^5 - 15t^4 + 10t^3)
inline double perlinEase (double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

inline double lerpDouble (double t, double a, double b) { return a + t * (b - a); }

inline double perlinNoise (double x, double y, double z) {
    int X = static_cast<int> (std::floor (x)) & 255;
    int Y = static_cast<int> (std::floor (y)) & 255;
    int Z = static_cast<int> (std::floor (z)) & 255;

    x -= std::floor (x);
    y -= std::floor (y);
    z -= std::floor (z);

    double u = perlinEase (x);
    double v = perlinEase (y);
    double w = perlinEase (z);

    int A = PERLIN_PERM[X] + Y;
    int AA = PERLIN_PERM[A] + Z;
    int AB = PERLIN_PERM[A + 1] + Z;
    int B = PERLIN_PERM[X + 1] + Y;
    int BA = PERLIN_PERM[B] + Z;
    int BB = PERLIN_PERM[B + 1] + Z;

    return lerpDouble (
	w,
	lerpDouble (
	    v, lerpDouble (u, perlinGrad (PERLIN_PERM[AA], x, y, z), perlinGrad (PERLIN_PERM[BA], x - 1, y, z)),
	    lerpDouble (u, perlinGrad (PERLIN_PERM[AB], x, y - 1, z), perlinGrad (PERLIN_PERM[BB], x - 1, y - 1, z))
	),
	lerpDouble (
	    v,
	    lerpDouble (
		u, perlinGrad (PERLIN_PERM[AA + 1], x, y, z - 1), perlinGrad (PERLIN_PERM[BA + 1], x - 1, y, z - 1)
	    ),
	    lerpDouble (
		u, perlinGrad (PERLIN_PERM[AB + 1], x, y - 1, z - 1),
		perlinGrad (PERLIN_PERM[BB + 1], x - 1, y - 1, z - 1)
	    )
	)
    );
}

// offset per axis so the 3 samples are decorrelated instead of identical
inline glm::vec3 perlinNoiseVec3 (const glm::vec3& p) {
    return glm::vec3 (
	static_cast<float> (perlinNoise (p.x, p.y, p.z)),
	static_cast<float> (perlinNoise (p.x + 89.2, p.y + 33.1, p.z + 57.3)),
	static_cast<float> (perlinNoise (p.x + 100.3, p.y + 120.1, p.z + 142.2))
    );
}

// Curl noise - smooth, swirling patterns ideal for fluid-like particle motion
inline glm::vec3 curlNoise (const glm::vec3& p) {
    const float e = 1e-4f;

    glm::vec3 dx (e, 0, 0);
    glm::vec3 dy (0, e, 0);
    glm::vec3 dz (0, 0, e);

    glm::vec3 x0 = perlinNoiseVec3 (p - dx);
    glm::vec3 x1 = perlinNoiseVec3 (p + dx);
    glm::vec3 y0 = perlinNoiseVec3 (p - dy);
    glm::vec3 y1 = perlinNoiseVec3 (p + dy);
    glm::vec3 z0 = perlinNoiseVec3 (p - dz);
    glm::vec3 z1 = perlinNoiseVec3 (p + dz);

    float x = (y1.z - y0.z) - (z1.y - z0.y);
    float y = (z1.x - z0.x) - (x1.z - x0.z);
    float z = (x1.y - x0.y) - (y1.x - y0.x);

    return glm::vec3 (x, y, z) / (2.0f * e);
}

// Stefan Gustavson's 1D simplex noise, the one wallpaper64.exe uses for turbulentvelocityrandom (roughly -1..1)
inline float simplexNoise1D (float x) {
    const int i0 = static_cast<int> (std::floor (x));
    const float x0 = x - static_cast<float> (i0);
    const float x1 = x0 - 1.0f;

    const auto grad = [] (int hash, float value) {
	const float gradient = 1.0f + static_cast<float> (hash & 7);
	return ((hash & 8) ? -gradient : gradient) * value;
    };

    float t0 = 1.0f - x0 * x0;
    t0 *= t0;
    float t1 = 1.0f - x1 * x1;
    t1 *= t1;

    const float n0 = t0 * t0 * grad (PERLIN_PERM[i0 & 0xff], x0);
    const float n1 = t1 * t1 * grad (PERLIN_PERM[(i0 + 1) & 0xff], x1);

    return 0.395f * (n0 + n1);
}

// Stefan Gustavson's 3D simplex noise, used by wallpaper64.exe's turbulence operator (sub_1401EB070, roughly -1..1)
inline float simplexNoise3D (float x, float y, float z) {
    static constexpr float gradients[12][3] = {
	{ 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 },
	{ 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 },
    };
    constexpr float F3 = 1.0f / 3.0f;
    constexpr float G3 = 1.0f / 6.0f;

    const float s = (x + y + z) * F3;
    const int i = static_cast<int> (std::floor (x + s));
    const int j = static_cast<int> (std::floor (y + s));
    const int k = static_cast<int> (std::floor (z + s));
    const float t = static_cast<float> (i + j + k) * G3;
    const float x0 = x - (static_cast<float> (i) - t);
    const float y0 = y - (static_cast<float> (j) - t);
    const float z0 = z - (static_cast<float> (k) - t);

    int i1, j1, k1, i2, j2, k2;

    if (x0 >= y0) {
	if (y0 >= z0) {
	    i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
	} else if (x0 >= z0) {
	    i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 0, k2 = 1;
	} else {
	    i1 = 0, j1 = 0, k1 = 1, i2 = 1, j2 = 0, k2 = 1;
	}
    } else {
	if (y0 < z0) {
	    i1 = 0, j1 = 0, k1 = 1, i2 = 0, j2 = 1, k2 = 1;
	} else if (x0 < z0) {
	    i1 = 0, j1 = 1, k1 = 0, i2 = 0, j2 = 1, k2 = 1;
	} else {
	    i1 = 0, j1 = 1, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
	}
    }

    const float offsets[4][3] = {
	{ x0, y0, z0 },
	{ x0 - i1 + G3, y0 - j1 + G3, z0 - k1 + G3 },
	{ x0 - i2 + 2.0f * G3, y0 - j2 + 2.0f * G3, z0 - k2 + 2.0f * G3 },
	{ x0 - 1.0f + 3.0f * G3, y0 - 1.0f + 3.0f * G3, z0 - 1.0f + 3.0f * G3 },
    };
    const int corners[4][3] = { { 0, 0, 0 }, { i1, j1, k1 }, { i2, j2, k2 }, { 1, 1, 1 } };
    const auto perm = [] (int index) { return static_cast<int> (PERLIN_PERM[index & 0xff]); };

    float total = 0.0f;

    for (int c = 0; c < 4; c++) {
	const float* o = offsets[c];
	float falloff = 0.6f - o[0] * o[0] - o[1] * o[1] - o[2] * o[2];

	if (falloff < 0.0f) {
	    continue;
	}

	const int gradient
	    = perm (i + corners[c][0] + perm (j + corners[c][1] + perm (k + corners[c][2]))) % 12;

	falloff *= falloff;
	total += falloff * falloff
	    * (gradients[gradient][0] * o[0] + gradients[gradient][1] * o[1] + gradients[gradient][2] * o[2]);
    }

    return 32.0f * total;
}

/**
 * wallpaper64.exe's 2D noise object (vtable off_140488440 slot 1, sub_1400FBE90, four lanes at once there): a hashed
 * simplex with the integer seed mixed into every corner hash, roughly -1..1. Same operations in the same order
 */
inline float hashedNoise2D (int32_t seed, float x, float y) {
    constexpr float F2 = 0.36602540f;
    constexpr float G2 = 0.21132487f;
    constexpr uint32_t primeX = 501125321u;
    constexpr uint32_t primeY = 1136930381u;
    constexpr uint32_t hashMultiplier = 0x27D4EB2Du;
    constexpr float cornerOffset = -0.57735026f;
    constexpr float gradientRatio = 2.4142137f;

    const float skew = (x + y) * F2;
    const float i = std::floor (skew + x);
    const float j = std::floor (skew + y);
    const float unskew = G2 * (j + i);
    const float x0 = x - (i - unskew);
    const float y0 = y - (j - unskew);
    const uint32_t hashX = static_cast<uint32_t> (static_cast<int32_t> (i)) * primeX;
    const uint32_t hashY = static_cast<uint32_t> (static_cast<int32_t> (j)) * primeY;

    const bool xFirst = y0 < x0;
    const float x1 = (x0 - (xFirst ? 1.0f : 0.0f)) + G2;
    const float y1 = (y0 - (xFirst ? 0.0f : 1.0f)) + G2;
    const float x2 = x0 + cornerOffset;
    const float y2 = y0 + cornerOffset;

    float a0 = std::max (-(x0 * x0) + (-(y0 * y0) + 0.5f), 0.0f);
    float a1 = std::max (-(x1 * x1) + (-(y1 * y1) + 0.5f), 0.0f);
    float a2 = std::max (-(x2 * x2) + (-(y2 * y2) + 0.5f), 0.0f);
    a0 *= a0;
    a1 *= a1;
    a2 *= a2;

    const auto hash = [seed] (uint32_t hx, uint32_t hy) {
	int32_t h = static_cast<int32_t> (((hy ^ hx) ^ static_cast<uint32_t> (seed)) * hashMultiplier);
	return h ^ (h >> 15);
    };
    const auto flip = [] (float value, int32_t bit) { return bit != 0 ? -value : value; };
    // bit 0 and 1 flip the signs of x and y, bit 2 decides which one gets the bigger weight
    const auto gradient = [&flip, gradientRatio] (int32_t h, float gx, float gy) {
	const float a = flip (gx, h & 1);
	const float b = flip (gy, (h >> 1) & 1);
	return (h & 4) != 0 ? b * gradientRatio + a : a * gradientRatio + b;
    };

    const int32_t h0 = hash (hashX, hashY);
    const int32_t h1 = hash (hashX + (xFirst ? primeX : 0u), hashY + (xFirst ? 0u : primeY));
    const int32_t h2 = hash (hashX + primeX, hashY + primeY);

    return (gradient (h0, x0, y0) * (a0 * a0) + (gradient (h1, x1, y1) * (a1 * a1) + gradient (h2, x2, y2) * (a2 * a2)))
	* 38.283688f;
}

/** 1 / (1 + 0.5 + 0.25 ...) over the octaves, what wallpaper64.exe scales its fbm by (off_140488440 slot 3) */
inline float hashedNoiseFbmNormalizer (int octaves) {
    float total = 1.0f;
    float amplitude = 0.5f;
    for (int octave = 1; octave < octaves; octave++) {
	total += amplitude;
	amplitude *= 0.5f;
    }
    return 1.0f / total;
}

/** off_140488440 slot 4 (sub_1400FBC70): octaves of hashedNoise2D, frequency x2, seed +1 and amplitude x0.5 each */
inline float hashedNoiseFbm (int octaves, float amplitude, int32_t seed, float x, float y) {
    float sum = hashedNoise2D (seed, x, y) * amplitude;
    for (int octave = 1; octave < octaves; octave++) {
	x *= 2.0f;
	y *= 2.0f;
	seed++;
	amplitude *= 0.5f;
	sum += hashedNoise2D (seed, x, y) * amplitude;
    }
    return sum;
}

/** wallpaper64.exe sub_14027B170: 2D simplex over the permutation table, roughly -1..1 */
inline float simplexNoise2D (float x, float y) {
    constexpr float F2 = 0.36602540f;
    constexpr float G2 = 0.21132487f;
    constexpr float G2x2 = 0.42264974f;

    const float skew = (x + y) * F2;
    const int i = static_cast<int> (std::floor (skew + x));
    const int j = static_cast<int> (std::floor (skew + y));
    const float unskew = static_cast<float> (j + i) * G2;
    const float x0 = x - (static_cast<float> (i) - unskew);
    const float y0 = y - (static_cast<float> (j) - unskew);
    const int i1 = x0 > y0 ? 1 : 0;
    const int j1 = 1 - i1;
    const float x1 = (x0 - static_cast<float> (i1)) + G2;
    const float y1 = (y0 - static_cast<float> (j1)) + G2;
    const float x2 = (x0 - 1.0f) + G2x2;
    const float y2 = (y0 - 1.0f) + G2x2;

    const auto perm = [] (int index) { return static_cast<int> (PERLIN_PERM[index & 0xff]); };
    // (h & 0x3c) swaps the axes, bit 0 negates the first one, bit 1 doubles the second one negative instead
    const auto corner = [] (int h, float cx, float cy, float falloff) {
	if (falloff < 0.0f) {
	    return 0.0f;
	}
	float u = cx;
	float v = cy;
	if ((h & 0x3c) != 0) {
	    std::swap (u, v);
	}
	if ((h & 1) != 0) {
	    u = -u;
	}
	v = (h & 2) != 0 ? v * -2.0f : v + v;
	falloff *= falloff;
	return (v + u) * (falloff * falloff);
    };

    const float n0 = corner (perm (i + perm (j)), x0, y0, (0.5f - x0 * x0) - y0 * y0);
    const float n1 = corner (perm (i1 + perm (j + j1) + i), x1, y1, (0.5f - x1 * x1) - y1 * y1);
    const float n2 = corner (perm (i + 1 + perm (j + 1)), x2, y2, (0.5f - x2 * x2) - y2 * y2);
    return ((n1 + n0) + n2) * 45.230652f;
}

/** wallpaper64.exe sub_14027B4B0: octaves of simplexNoise1D, frequency x2 and amplitude x0.5 each, averaged */
inline float simplexFbm1D (float x, float frequency, int octaves) {
    float sum = 0.0f;
    float total = 0.0f;
    float amplitude = 1.0f;
    for (int octave = 0; octave < octaves; octave++) {
	sum += simplexNoise1D (frequency * x) * amplitude;
	total += amplitude;
	frequency *= 2.0f;
	amplitude *= 0.5f;
    }
    return sum / total;
}

} // namespace WallpaperEngine::Render::Utils
