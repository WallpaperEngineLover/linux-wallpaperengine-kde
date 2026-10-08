#pragma once

#include <optional>
#include <vector>

namespace WallpaperEngine::VideoPlayback {
/** In seconds, no end means up to the end of the video */
struct VideoSegment {
    double start = 0.0;
    std::optional<double> end;

    bool operator== (const VideoSegment& other) const = default;
};

/** Sorted and non-overlapping, empty plays it all */
using VideoSegments = std::vector<VideoSegment>;
} // namespace WallpaperEngine::VideoPlayback
