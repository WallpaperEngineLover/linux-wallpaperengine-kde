#pragma once

#include <atomic>

#include "ApplicationContext.h"

namespace WallpaperEngine::Application {
enum class XrayMode {
    Normal,
    Full,
    Disabled,
};

class ApplicationState {
public:
    struct {
	std::atomic<bool> keepRunning;
    } general {};

    struct {
	bool enabled;
	int volume;
    } audio {};

    struct {
	bool enabled;
    } mouse {};

    struct {
	XrayMode mode;
    } xray {};
};
} // namespace WallpaperEngine::Application