#pragma once

#include "ApplicationContext.h"

namespace WallpaperEngine::Application {
/**
 * Represents current application state
 */
class ApplicationState {
public:
    struct {
	bool keepRunning;
    } general {};

    struct {
	bool enabled;
	int volume;
    } audio {};

    struct {
	bool enabled;
    } mouse {};

    struct {
	/** If true, xray-style effects (Wallpaper Engine's built-in "effects/xray") render fully revealed
	 * everywhere instead of following the mouse pointer */
	bool fullReveal;
    } xray {};
};
} // namespace WallpaperEngine::Application