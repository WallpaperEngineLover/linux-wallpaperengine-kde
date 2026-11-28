#pragma once

#include <cstdint>
#include <glm/vec4.hpp>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace WallpaperEngine::Application {
class ApplicationContext;
}

namespace WallpaperEngine::Desktop {
/** A window reported by the GNOME extension or KWin script, filtered by WE 2.8's rules (sub_14003D050) */
struct WindowState {
    enum Flags : uint32_t {
	Focused = 1,
	Maximized = 2,
	/** focused, covers its monitor, not maximized */
	Fullscreen = 4,
    };

    std::string appId;
    std::string output;
    uint32_t flags = 0;
};

bool containsIgnoringCase (std::string_view haystack, std::string_view needle);

/** The outputs a window at x, y, width, height (a glm vec4) is on: every output it covers at least half of, else the
 * one that holds its centre, none when it is off all of them */
std::vector<std::string> outputsCoveredBy (const glm::ivec4& window, const std::map<std::string, glm::ivec4>& outputs);

bool pausesAnyOf (
    const std::vector<WindowState>& windows, const std::vector<std::string>& outputs,
    const Application::ApplicationContext& context
);
}
