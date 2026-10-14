#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace WallpaperEngine::Data::Model {
struct AnimationKeyframe {
    struct Handle {
	bool enabled = false;
	float x = 0.0f;
	float y = 0.0f;
    };

    /** Whole frames, WE reads them as integers */
    float frame = 0.0f;
    float value = 0.0f;
    Handle back;
    Handle front;
    /** Holds the previous key's value up to this one */
    bool step = false;
};

struct AnimationEvent {
    float frame = 0.0f;
    std::string name;
};

// a property's "animation" keyframes, one curve per component (c0..c3), empty curves leave it alone; linked properties
// share the root's playback
struct PropertyAnimation {
    enum class Mode { Single, Loop, Mirror };

    std::array<std::vector<AnimationKeyframe>, 4> curves;
    std::string name;
    float fps = 30.0f;
    float length = 0.0f;
    Mode mode = Mode::Single;
    bool startPaused = false;
    bool wrapLoop = false;
    std::vector<AnimationEvent> events;
    std::vector<std::string> children;
    std::optional<std::string> parent;
};
} // namespace WallpaperEngine::Data::Model
