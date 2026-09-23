#include "SpectrumNormalizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

using namespace WallpaperEngine::Audio;

namespace {
// levels are 0.35 * log10(power) scaled, so one unit is a bit under 30dB
constexpr float WINDOW = 1.2f;
// below this a band is never shown, capture noise and dither sit around here
constexpr float GATE = 0.2f;
// the smallest span that still gets stretched to fill the range, so faint sounds fill it without noise blowing up
constexpr float MIN_SPAN = 0.35f;
// how quickly the reference rises to a louder peak, per second (as an exponential rate)
constexpr float ATTACK_RATE = 25.0f;
constexpr float RELEASE_SECONDS = 3.0f;
} // namespace

void SpectrumNormalizer::update (const float* levels, std::size_t count, float dt) {
    dt = std::clamp (dt, 0.0f, 0.25f);
    count = std::min<std::size_t> (count, 64);

    if (count == 0) {
	return;
    }

    std::array<float, 64> sorted {};
    std::copy_n (levels, count, sorted.begin ());

    // average of the loudest fifth of the bands
    const std::size_t top = std::max<std::size_t> (1, count / 5);
    std::partial_sort (sorted.begin (), sorted.begin () + top, sorted.begin () + count, std::greater<> ());

    float peak = 0.0f;
    for (std::size_t i = 0; i < top; i++) {
	peak += sorted[i];
    }
    peak /= static_cast<float> (top);

    if (peak > this->m_reference) {
	this->m_reference += (peak - this->m_reference) * (1.0f - std::exp (-ATTACK_RATE * dt));
    } else {
	// falls back to the current peak rather than to zero, so a steady level settles right at it
	this->m_reference = peak + (this->m_reference - peak) * std::exp (-dt / RELEASE_SECONDS);
    }
}

float SpectrumNormalizer::apply (float level) const {
    const float high = std::max (this->m_reference, GATE + MIN_SPAN);
    const float low = std::max (high - WINDOW, GATE);

    return std::clamp ((level - low) / (high - low), 0.0f, 1.0f);
}
