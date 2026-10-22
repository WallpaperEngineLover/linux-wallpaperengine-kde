#pragma once

#include <glm/vec4.hpp>
#include <string>
#include <vector>

#include "FullScreenDetector.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"
#include <X11/Xlib.h>

namespace WallpaperEngine::Render::Drivers {
class GLFWOpenGLDriver;

namespace Detectors {
    class X11FullScreenDetector final : public FullScreenDetector {
    public:
	X11FullScreenDetector (Application::ApplicationContext& appContext, VideoDriver& driver);
	~X11FullScreenDetector () override;

	[[nodiscard]] bool anythingFullscreen () const override;
	void reset () override;

    private:
	void initialize ();
	[[nodiscard]] bool isDesktopWindow (Window window) const;
	[[nodiscard]] Window clientOf (Window window) const;
	[[nodiscard]] bool hasFullscreenState (Window client) const;
	[[nodiscard]] bool hasMaximizedState (Window client) const;
	[[nodiscard]] bool isHidden (Window client) const;
	[[nodiscard]] bool onCurrentDesktop (Window client) const;
	[[nodiscard]] bool coversScreen (Window client) const;
	[[nodiscard]] bool applicationFocused (Window ourWindow) const;
	void stop ();

	Display* m_display = nullptr;
	Window m_root;
	std::map<std::string, glm::ivec4> m_screens = {};
	VideoDriver& m_driver;
    };
}
}