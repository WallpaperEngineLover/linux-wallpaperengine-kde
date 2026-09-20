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

    float frame = 0.0f;
    float value = 0.0f;
    Handle back;
    Handle front;
};

struct AnimationEvent {
    float frame = 0.0f;
    std::string name;
};

/**
 * Keyframe timeline stored under a property's "animation" key. One curve per vector component
 * (c0..c3); a component with no keyframes is left alone. Properties linked through
 * children/parent share the playback state of the root one.
 */
struct PropertyAnimation {
    enum class Mode { Single, Loop, Mirror };

    std::array<std::vector<AnimationKeyframe>, 4> curves;
    std::string name;
    float fps = 30.0f;
    float length = 0.0f;
    Mode mode = Mode::Single;
    bool startPaused = false;
    bool wrapLoop = false;
    /** Curve values are offsets on top of the property's own value instead of absolute values */
    bool relative = false;
    std::vector<AnimationEvent> events;
    std::vector<std::string> children;
    std::optional<std::string> parent;
};
} // namespace WallpaperEngine::Data::Model
