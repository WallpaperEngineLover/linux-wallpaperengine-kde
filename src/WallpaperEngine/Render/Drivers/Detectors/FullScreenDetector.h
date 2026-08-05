#pragma once

#include "WallpaperEngine/Application/ApplicationContext.h"

namespace WallpaperEngine::Render::Drivers::Detectors {
class FullScreenDetector {
public:
    explicit FullScreenDetector (Application::ApplicationContext& appContext);
    virtual ~FullScreenDetector () = default;

    [[nodiscard]] virtual bool anythingFullscreen () const;
    /** Resets the detector, useful when resources tied to the output driver need to be released */
    virtual void reset ();
    [[nodiscard]] Application::ApplicationContext& getApplicationContext () const;

private:
    Application::ApplicationContext& m_applicationContext;
};
} // namespace WallpaperEngine::Render::Drivers::Detectors