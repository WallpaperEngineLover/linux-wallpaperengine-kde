#pragma once

#include <cstdint>
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
    /** wl_output name */
    std::string output;
    uint32_t flags = 0;
};

/** ASCII case-insensitive, for --fullscreen-pause-ignore-appid */
bool containsIgnoringCase (std::string_view haystack, std::string_view needle);

/** WE's per monitor pause: true when a pausing window is on one of outputs */
bool pausesAnyOf (
    const std::vector<WindowState>& windows, const std::vector<std::string>& outputs,
    const Application::ApplicationContext& context
);
} // namespace WallpaperEngine::Desktop
