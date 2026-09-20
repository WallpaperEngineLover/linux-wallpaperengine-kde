#include "ThumbnailPalette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace WallpaperEngine::Media;

namespace {
constexpr int kBucketBits = 4;
constexpr int kBucketsPerChannel = 1 << kBucketBits;
// colors closer than this (euclidean distance in 0-1 RGB) count as the same color
constexpr float kMinDistinctDistance = 0.28f;

struct Bucket {
    uint32_t count = 0;
    glm::vec3 sum = { 0.0f, 0.0f, 0.0f };

    [[nodiscard]] glm::vec3 average () const { return sum / static_cast<float> (count); }
};

float luminance (const glm::vec3& color) {
    const auto linear = [] (float channel) {
	return channel <= 0.03928f ? channel / 12.92f : std::pow ((channel + 0.055f) / 1.055f, 2.4f);
    };

    return 0.2126f * linear (color.r) + 0.7152f * linear (color.g) + 0.0722f * linear (color.b);
}

float contrastRatio (const glm::vec3& a, const glm::vec3& b) {
    const float la = luminance (a);
    const float lb = luminance (b);

    return (std::max (la, lb) + 0.05f) / (std::min (la, lb) + 0.05f);
}
} // namespace

ThumbnailPalette WallpaperEngine::Media::computeThumbnailPalette (const uint8_t* rgba, size_t width, size_t height) {
    ThumbnailPalette palette;

    if (rgba == nullptr || width == 0 || height == 0) {
	return palette;
    }

    std::array<Bucket, kBucketsPerChannel * kBucketsPerChannel * kBucketsPerChannel> buckets;

    // looking at every pixel of a 3000px cover is pointless, a ~96x96 grid is plenty
    const size_t stepX = std::max<size_t> (1, width / 96);
    const size_t stepY = std::max<size_t> (1, height / 96);

    for (size_t y = 0; y < height; y += stepY) {
	for (size_t x = 0; x < width; x += stepX) {
	    const uint8_t* pixel = rgba + (y * width + x) * 4;

	    if (pixel[3] < 128) {
		continue;
	    }

	    const size_t index = ((static_cast<size_t> (pixel[0]) >> (8 - kBucketBits)) * kBucketsPerChannel
				  + (static_cast<size_t> (pixel[1]) >> (8 - kBucketBits)))
		    * kBucketsPerChannel
		+ (static_cast<size_t> (pixel[2]) >> (8 - kBucketBits));

	    buckets[index].count++;
	    buckets[index].sum += glm::vec3 (pixel[0], pixel[1], pixel[2]) / 255.0f;
	}
    }

    std::vector<const Bucket*> ordered;

    for (const auto& bucket : buckets) {
	if (bucket.count > 0) {
	    ordered.push_back (&bucket);
	}
    }

    if (ordered.empty ()) {
	return palette;
    }

    std::ranges::sort (ordered, [] (const Bucket* a, const Bucket* b) { return a->count > b->count; });

    std::vector<glm::vec3> picked;

    for (const Bucket* bucket : ordered) {
	const glm::vec3 color = bucket->average ();
	const bool distinct = std::ranges::all_of (picked, [&color] (const glm::vec3& other) {
	    return glm::distance (color, other) >= kMinDistinctDistance;
	});

	if (distinct) {
	    picked.push_back (color);
	}

	if (picked.size () == 3) {
	    break;
	}
    }

    // flat covers only have one or two distinct colors, reuse what there is
    while (picked.size () < 3) {
	picked.push_back (picked.back ());
    }

    palette.primary = picked[0];
    palette.secondary = picked[1];
    palette.tertiary = picked[2];

    const glm::vec3 white (1.0f);
    const glm::vec3 black (0.0f);

    palette.highContrast = contrastRatio (palette.primary, white) >= contrastRatio (palette.primary, black) ? white : black;
    palette.text = palette.highContrast;

    // a color from the cover itself is nicer than plain black/white as long as it is readable
    float bestContrast = 4.5f;

    for (const glm::vec3& candidate : { palette.secondary, palette.tertiary }) {
	const float contrast = contrastRatio (palette.primary, candidate);

	if (contrast > bestContrast) {
	    bestContrast = contrast;
	    palette.text = candidate;
	}
    }

    return palette;
}
