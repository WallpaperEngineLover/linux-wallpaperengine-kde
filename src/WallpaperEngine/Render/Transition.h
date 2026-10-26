#pragma once

#include <optional>
#include <string>

namespace WallpaperEngine::Render {
enum class TransitionKind { None, Fade, SlideLeft, SlideRight, WipeLeft, WipeRight, Zoom };

inline std::optional<TransitionKind> parseTransition (const std::string& value) {
    if (value == "none") {
	return TransitionKind::None;
    }
    if (value == "fade") {
	return TransitionKind::Fade;
    }
    if (value == "slide-left") {
	return TransitionKind::SlideLeft;
    }
    if (value == "slide-right") {
	return TransitionKind::SlideRight;
    }
    if (value == "wipe-left") {
	return TransitionKind::WipeLeft;
    }
    if (value == "wipe-right") {
	return TransitionKind::WipeRight;
    }
    if (value == "zoom") {
	return TransitionKind::Zoom;
    }
    return std::nullopt;
}

constexpr int TRANSITION_MIN_MS = 100;
constexpr int TRANSITION_MAX_MS = 5000;
}
