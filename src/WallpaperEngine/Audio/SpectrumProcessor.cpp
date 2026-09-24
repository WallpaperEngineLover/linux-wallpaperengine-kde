#include "SpectrumProcessor.h"

#include <algorithm>
#include <cmath>

using namespace WallpaperEngine::Audio;

namespace {
constexpr float SILENCE = 0.0001f;
} // namespace

void SpectrumProcessor::update (const float* raw, float dt) {
    dt = std::clamp (dt, SILENCE, 0.25f);

    float groupPeak[GROUPS];
    float peak = 0.0f;

    for (int g = 0; g < GROUPS; g++) {
	groupPeak[g] = *std::max_element (raw + g * 8, raw + g * 8 + 8);
	peak = std::max (peak, groupPeak[g]);
    }

    // a quiet group is measured against a third of the loudest one, so it doesn't get stretched to full range
    for (float& value : groupPeak) {
	value = std::max (value, peak * 0.333f);
    }

    if (!(this->m_level[0] > SILENCE) && peak >= SILENCE) {
	std::fill_n (this->m_level, GROUPS, 1.0f);
    }

    // the level walks towards the group's peak linearly, up at 1 per second and down at half that
    const float step = std::min (dt, 1.0f);

    for (int g = 0; g < GROUPS; g++) {
	const float diff = groupPeak[g] - this->m_level[g];

	if (std::fabs (diff) <= SILENCE) {
	    this->m_level[g] = groupPeak[g];
	} else {
	    this->m_level[g] += std::min (step, std::fabs (diff)) * (diff > 0.0f ? 1.0f : -0.5f);
	}
    }

    float* left = this->audio64;
    float* right = this->audio64 + 64;
    float* average = this->audio64 + 128;

    if (peak >= SILENCE) {
	const float follow = std::min (dt * 20.0f, 1.0f);
	const float maxStep = std::min (dt * 40.0f, 1.0f);

	for (int i = 0; i < 128; i++) {
	    const float scaled = raw[i] / std::max (this->m_level[i / 8], 0.001f);

	    this->m_smoothed[i] += (scaled - this->m_smoothed[i]) * follow;
	    this->m_output[i] += std::clamp (this->m_smoothed[i] - this->m_output[i], -maxStep, maxStep);
	}

	std::copy_n (this->m_output, 128, this->audio64);
    } else {
	// silence drops straight to zero, the smoothing state is kept for when sound comes back
	std::fill_n (this->audio64, 128, 0.0f);
    }

    for (int i = 0; i < 64; i++) {
	average[i] = (left[i] + right[i]) * 0.5f;
    }

    for (int i = 0; i < 32 * 3; i++) {
	this->audio32[i] = std::max (this->audio64[i * 2], this->audio64[i * 2 + 1]);
    }

    for (int i = 0; i < 16 * 3; i++) {
	this->audio16[i] = std::max (this->audio32[i * 2], this->audio32[i * 2 + 1]);
    }
}
