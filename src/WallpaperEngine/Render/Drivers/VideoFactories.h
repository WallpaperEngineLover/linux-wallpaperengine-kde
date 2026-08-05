#pragma once

#include <functional>
#include <map>
#include <memory>

#include "VideoDriver.h"

#define DEFAULT_WINDOW_NAME "default"

namespace WallpaperEngine::Render::Drivers {
class VideoFactories {
public:
    using DriverConstructionFunc
	= std::function<std::unique_ptr<VideoDriver> (ApplicationContext&, WallpaperApplication&)>;
    using FullscreenDetectorConstructionFunc
	= std::function<std::unique_ptr<Detectors::FullScreenDetector> (ApplicationContext&, VideoDriver&)>;
    VideoFactories ();
    ~VideoFactories () = default;

    static VideoFactories& get ();

    void registerDriver (
	ApplicationContext::WINDOW_MODE forMode, std::string xdgSessionType, DriverConstructionFunc factory
    );

    void registerFullscreenDetector (std::string xdgSessionType, FullscreenDetectorConstructionFunc factory);

    [[nodiscard]] std::vector<std::string> getRegisteredDrivers () const;

    [[nodiscard]] std::unique_ptr<VideoDriver> createVideoDriver (
	ApplicationContext::WINDOW_MODE mode, const std::string& xdgSessionType, ApplicationContext& context,
	WallpaperApplication& application
    );

    /** Falls back to a no-op FullScreenDetector when no factory is registered for xdgSessionType */
    [[nodiscard]] std::unique_ptr<Detectors::FullScreenDetector>
    createFullscreenDetector (const std::string& xdgSessionType, ApplicationContext& context, VideoDriver& driver);

private:
    using SessionTypeToFullscreenDetectorType = std::map<std::string, FullscreenDetectorConstructionFunc>;
    using SessionTypeToFactoryType = std::map<std::string, DriverConstructionFunc>;
    using WindowModeToSessionType = std::map<ApplicationContext::WINDOW_MODE, SessionTypeToFactoryType>;

    SessionTypeToFullscreenDetectorType m_fullscreenFactories = {};
    WindowModeToSessionType m_driverFactories = {};
    static std::unique_ptr<VideoFactories> sInstance;
};
} // namespace WallpaperEngine::Render::Drivers

#define sVideoFactories (WallpaperEngine::Render::Drivers::VideoFactories::get ())