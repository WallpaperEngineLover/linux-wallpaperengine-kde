#pragma once

#include <glm/vec4.hpp>
#include <memory>
#include <vector>

#include "TextureCache.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Input/InputContext.h"
#include "WallpaperEngine/Input/MouseInput.h"
#include "WallpaperEngine/Render/Drivers/Output/Output.h"
#include "WallpaperEngine/Render/Drivers/Output/OutputViewport.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"

namespace WallpaperEngine {
namespace Application {
    class WallpaperApplication;
}
namespace Media {
    class MediaSource;
}

namespace Render {
    namespace Drivers {
	class VideoDriver;

	namespace Output {
	    class Output;
	    class OutputViewport;
	} // namespace Output
    } // namespace Drivers

    class CWallpaper;
    class TextureCache;

    class RenderContext {
    public:
	RenderContext (Drivers::VideoDriver& driver, WallpaperApplication& app, Media::MediaSource& mediaSource);

	void render (Drivers::Output::OutputViewport* viewport);
	void setWallpaper (const std::string& display, std::shared_ptr<CWallpaper> wallpaper);
	void setPause (bool newState) const;
	[[nodiscard]] Input::InputContext& getInputContext () const;
	[[nodiscard]] const WallpaperApplication& getApp () const;
	[[nodiscard]] const Drivers::VideoDriver& getDriver () const;
	[[nodiscard]] const Drivers::Output::Output& getOutput () const;
	[[nodiscard]] std::shared_ptr<const TextureProvider> resolveTexture (const std::string& name) const;
	[[nodiscard]] const std::map<std::string, std::shared_ptr<CWallpaper>>& getWallpapers () const;
	[[nodiscard]] Media::MediaSource& getMediaSource () const;

    private:
	Drivers::VideoDriver& m_driver;
	std::map<std::string, std::shared_ptr<CWallpaper>> m_wallpapers = {};
	WallpaperApplication& m_app;
	Media::MediaSource& m_mediaSource;
	std::unique_ptr<TextureCache> m_textureCache = nullptr;
    };
} // namespace Render
} // namespace WallpaperEngine
