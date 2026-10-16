#include "SplitLayout.h"

#include <cmath>
#include <numeric>

namespace WallpaperEngine::Render {
std::vector<glm::ivec4> splitSlotRects (
    const glm::ivec2& size, const std::vector<float>& aspects,
    const Application::ApplicationContext::SplitScreen::Layout layout, const bool yUp
) {
    using SplitScreen = Application::ApplicationContext::SplitScreen;

    const auto count = aspects.size ();

    if (count == 0 || size.x <= 0 || size.y <= 0) {
	return std::vector<glm::ivec4> (count, glm::ivec4 (0));
    }

    // portrait screens stack the slots
    const bool columns = size.x >= size.y;
    const double length = columns ? size.x : size.y;
    const double across = columns ? size.y : size.x;

    std::vector<double> weights (count, 0.0);
    double known = 0.0;
    int knownCount = 0;

    for (std::size_t i = 0; i < count; i++) {
	if (aspects[i] > 0.0f && std::isfinite (aspects[i])) {
	    weights[i] = columns ? aspects[i] : 1.0 / aspects[i];
	    known += weights[i];
	    knownCount++;
	}
    }

    // sizeless pages get the average, or an equal share
    const double fallback = knownCount > 0 ? known / knownCount : length / across / count;

    for (auto& weight : weights) {
	if (weight <= 0.0 || layout == SplitScreen::LAYOUT_EQUAL) {
	    weight = layout == SplitScreen::LAYOUT_EQUAL ? 1.0 : fallback;
	}
    }

    const double total = std::accumulate (weights.begin (), weights.end (), 0.0);
    double scale = length / total;
    double start = 0.0;

    if (layout == SplitScreen::LAYOUT_CENTER && total * across <= length) {
	scale = across;
	start = (length - total * across) / 2.0;
    }

    std::vector<glm::ivec4> rects;
    rects.reserve (count);
    double edge = start;

    for (const auto weight : weights) {
	const int from = static_cast<int> (std::lround (edge));
	edge += weight * scale;
	const int to = static_cast<int> (std::lround (edge));

	if (columns) {
	    rects.emplace_back (from, 0, to - from, size.y);
	} else {
	    const int top = yUp ? size.y - to : from;
	    rects.emplace_back (0, top, size.x, to - from);
	}
    }

    return rects;
}
} // namespace WallpaperEngine::Render
