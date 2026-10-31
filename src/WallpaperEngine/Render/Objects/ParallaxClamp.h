#pragma once

#include <algorithm>

namespace WallpaperEngine::Render::Objects {
// keeps an edge pair (m_pos.x/.z) from sliding into the visible canvas, so the image never uncovers ground it has no
// pixels for; images too small to cover the axis move freely and cropped canvas may scroll in. WE has no clamp
// (sub_14018AAC0)
inline float clampParallaxAxis (float offset, float edgeA, float edgeB, float visibleLow, float visibleHigh) {
    const float low = std::min (edgeA, edgeB);
    const float high = std::max (edgeA, edgeB);
    const float maxOffset = visibleLow - low;
    const float minOffset = visibleHigh - high;

    if (minOffset > maxOffset) {
	return offset;
    }

    // a layer that doesn't cover the screen at rest stays where the scene puts it (3621923790's hair), the clamp
    // only keeps the parallax from uncovering more
    return std::clamp (offset, std::min (minOffset, 0.0f), std::max (maxOffset, 0.0f));
}
}
