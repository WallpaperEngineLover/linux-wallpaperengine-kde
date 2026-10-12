#include "WindowState.h"

#include "WallpaperEngine/Application/ApplicationContext.h"

#include <algorithm>
#include <cctype>

using namespace WallpaperEngine::Desktop;

namespace {
bool containsIgnoringCase (const std::string& haystack, const std::string& needle) {
    return std::ranges::search (
	       haystack, needle, [] (unsigned char a, unsigned char b) { return std::tolower (a) == std::tolower (b); }
	   ).begin ()
	!= haystack.end ();
}
} // namespace

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
