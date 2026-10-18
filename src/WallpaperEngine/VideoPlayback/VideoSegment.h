#pragma once

#include <optional>
#include <vector>

namespace WallpaperEngine::VideoPlayback {
struct VideoSegment {
    double start = 0.0;
    std::optional<double> end;

    bool operator== (const VideoSegment& other) const = default;
};

using VideoSegments = std::vector<VideoSegment>;
}
