#include "WindowState.h"

#include "WallpaperEngine/Application/ApplicationContext.h"

#include <algorithm>
#include <cctype>

using namespace WallpaperEngine::Desktop;

bool WallpaperEngine::Desktop::containsIgnoringCase (std::string_view haystack, std::string_view needle) {
    return std::ranges::search (
	       haystack, needle, [] (unsigned char a, unsigned char b) { return std::tolower (a) == std::tolower (b); }
	   ).begin ()
	!= haystack.end ();
}

std::vector<std::string> WallpaperEngine::Desktop::outputsCoveredBy (
    const glm::ivec4& window, const std::map<std::string, glm::ivec4>& outputs
) {
    std::vector<std::string> covered;
    std::string holdingCentre;
    const int centreX = window.x + window.z / 2;
    const int centreY = window.y + window.w / 2;

    for (const auto& [name, area] : outputs) {
	const long long width = std::min (window.x + window.z, area.x + area.z) - std::max (window.x, area.x);
	const long long height = std::min (window.y + window.w, area.y + area.w) - std::max (window.y, area.y);

	if (width > 0 && height > 0 && 2 * width * height >= static_cast<long long> (area.z) * area.w) {
	    covered.push_back (name);
	}

	if (centreX >= area.x && centreX < area.x + area.z && centreY >= area.y && centreY < area.y + area.w) {
	    holdingCentre = name;
	}
    }

    if (covered.empty () && !holdingCentre.empty ()) {
	covered.push_back (holdingCentre);
    }

    return covered;
}

bool WallpaperEngine::Desktop::pausesAnyOf (
    const std::vector<WindowState>& windows, const std::vector<std::string>& outputs,
    const Application::ApplicationContext& context
) {
    const auto& settings = context.settings.render;
    const uint32_t pausing = (settings.pauseOnFullscreen ? WindowState::Maximized | WindowState::Fullscreen : 0)
	| (settings.pauseOnFocus ? WindowState::Focused : 0);

    return std::ranges::any_of (windows, [&] (const WindowState& window) {
	if ((window.flags & pausing) == 0) {
	    return false;
	}

	if (settings.pauseOnFullscreenOnlyWhenActive && (window.flags & WindowState::Focused) == 0) {
	    return false;
	}

	if (std::ranges::any_of (settings.fullscreenPauseIgnoreAppIds, [&window] (const std::string& ignore) {
		return !ignore.empty () && containsIgnoringCase (window.appId, ignore);
	    })) {
	    return false;
	}

	return std::ranges::find (outputs, window.output) != outputs.end ();
    });
}
