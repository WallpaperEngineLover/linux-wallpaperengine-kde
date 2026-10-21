#pragma once

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Render/Drivers/Detectors/FullScreenDetector.h"
#include <atomic>

namespace WallpaperEngine {
namespace Application {
    class ApplicationContext;
}

namespace Render::Drivers::Detectors {
    class FullScreenDetector;
}

namespace Audio::Drivers::Detectors {
    class AudioPlayingDetector {
    public:
	AudioPlayingDetector (
	    Application::ApplicationContext& appContext,
	    const Render::Drivers::Detectors::FullScreenDetector& fullscreenDetector
	);

	virtual ~AudioPlayingDetector () = default;

	[[nodiscard]] bool anythingPlaying () const;

	void setIsPlaying (bool newState);

	virtual void update ();

    protected:
	[[nodiscard]] Application::ApplicationContext& getApplicationContext () const;
	[[nodiscard]] const Render::Drivers::Detectors::FullScreenDetector& getFullscreenDetector () const;

    private:
	std::atomic<bool> m_isPlaying = false;

	Application::ApplicationContext& m_applicationContext;
	const Render::Drivers::Detectors::FullScreenDetector& m_fullscreenDetector;
    };
}
}
