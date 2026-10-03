#pragma once

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Render/Drivers/Detectors/FullScreenDetector.h"

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

	/**
	 * @return If any kind of sound is currently playing on the default audio device
	 */
	[[nodiscard]] bool anythingPlaying () const;

	void setIsPlaying (bool newState);

	virtual void update ();

    protected:
	[[nodiscard]] Application::ApplicationContext& getApplicationContext () const;
	[[nodiscard]] const Render::Drivers::Detectors::FullScreenDetector& getFullscreenDetector () const;

    private:
	bool m_isPlaying = false;

	Application::ApplicationContext& m_applicationContext;
	const Render::Drivers::Detectors::FullScreenDetector& m_fullscreenDetector;
    };
} // namespace Audio::Drivers::Detectors
} // namespace WallpaperEngine
