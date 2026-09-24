#pragma once

#include <cmath>
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

} // namespace WallpaperEngine::Render::Utils
