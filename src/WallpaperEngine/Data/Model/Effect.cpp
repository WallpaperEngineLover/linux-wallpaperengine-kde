#include "Effect.h"

#include <algorithm>

using namespace WallpaperEngine::Data::Model;

bool EffectConditions::holds (const ComboMap& combos) const {
    return std::ranges::all_of (this->tests, [&combos] (const EffectCondition& test) {
	const auto it = combos.find (test.combo);
	const int combo = it != combos.end () ? it->second : 0;

	switch (test.op) {
	    case EffectCondition::GreaterEqual:
		return combo >= test.value;
	    case EffectCondition::Greater:
		return combo > test.value;
	    case EffectCondition::LessEqual:
		return combo <= test.value;
	    case EffectCondition::Less:
		return combo < test.value;
	    default:
		return combo == test.value;
	}
    });
}

// sub_1401EA500 picks the size, sub_1400D2C60 divides it by the scale
glm::uvec2 FBO::bufferSize (const glm::vec2 objectSize) const {
    int bufferWidth
	= this->width.has_value () ? static_cast<int> (*this->width) : std::max (static_cast<int> (objectSize.x), 4);
    int bufferHeight
	= this->height.has_value () ? static_cast<int> (*this->height) : std::max (static_cast<int> (objectSize.y), 4);

    if (this->fit.has_value () && bufferWidth > 0 && bufferHeight > 0) {
	const int fitSide = static_cast<int> (*this->fit);

	if (bufferWidth < bufferHeight) {
	    const int side = std::min (fitSide, bufferHeight);
	    bufferWidth = static_cast<int> (
		static_cast<float> (bufferWidth) / static_cast<float> (bufferHeight) * static_cast<float> (side)
	    );
	    bufferHeight = side;
	} else {
	    const int side = std::min (fitSide, bufferWidth);
	    bufferHeight = static_cast<int> (
		static_cast<float> (bufferHeight) / static_cast<float> (bufferWidth) * static_cast<float> (side)
	    );
	    bufferWidth = side;
	}
    }

    const int divisor = std::max (static_cast<int> (this->scale), 1);

    return { std::max (bufferWidth / divisor, 2), std::max (bufferHeight / divisor, 2) };
}
