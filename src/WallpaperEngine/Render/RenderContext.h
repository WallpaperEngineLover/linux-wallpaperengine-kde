#pragma once

#include <glm/vec4.hpp>
#include <memory>
#include <optional>
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
	}
    }

    class CWallpaper;
    class TextureCache;

    class RenderContext {
    public:
	RenderContext (Drivers::VideoDriver& driver, WallpaperApplication& app, Media::MediaSource& mediaSource);

	void render (Drivers::Output::OutputViewport* viewport);
	/** holdPrevious keeps the old wallpaper's last frame on screen until the new one has content, animate then
	 *  plays the configured transition from that frame to the new wallpaper */
	void setWallpaper (
	    const std::string& display, std::shared_ptr<CWallpaper> wallpaper, bool holdPrevious = false,
	    bool animate = false
	);
	void setPause (bool newState) const;
	void pruneTextures () const;
	void beginTexturePrefetch (
	    const Data::Model::Project& project,
	    std::function<void (const std::function<bool (std::vector<std::string>)>& found)> names
	) const;
	void endTexturePrefetch () const;
	void queueBackgroundTask (std::function<void ()> task) const;
	[[nodiscard]] Input::InputContext& getInputContext () const;
	[[nodiscard]] const WallpaperApplication& getApp () const;
	[[nodiscard]] const Drivers::VideoDriver& getDriver () const;
	[[nodiscard]] const Drivers::Output::Output& getOutput () const;
	[[nodiscard]] std::shared_ptr<const TextureProvider>
	resolveTexture (const std::string& name, const Data::Model::Project& project) const;
	[[nodiscard]] std::string iconTextureName (const std::filesystem::path& icon) const;
	[[nodiscard]] std::shared_ptr<const TextureProvider>
	findLoadedTexture (const std::string& name, const Data::Model::Project& project) const;
	[[nodiscard]] const std::map<std::string, std::shared_ptr<CWallpaper>>& getWallpapers () const;
	[[nodiscard]] Media::MediaSource& getMediaSource () const;
	[[nodiscard]] std::optional<glm::ivec4> getSlotRect (const std::string& slot) const;

    private:
	void renderWithStats (Drivers::Output::OutputViewport* viewport);
	void beginProfiledFrame ();
	void drawWallpapers (Drivers::Output::OutputViewport& viewport);
	void drawSplitScreen (
	    Drivers::Output::OutputViewport& viewport, const Application::ApplicationContext::SplitScreen& split
	);
	void holdFrame (const std::string& display, bool animate);
	void releaseHeldFrame (const std::string& display);
	void drawHeldFrame (const std::string& display, const glm::ivec4& rect, const CWallpaper& wallpaper);

	struct DrawTarget {
	    Drivers::Output::OutputViewport* viewport = nullptr;
	    glm::ivec4 rect {};
	};

	[[nodiscard]] std::optional<DrawTarget> findDrawTarget (const std::string& display) const;

	struct HeldFrame {
	    GLuint framebuffer = GL_NONE;
	    GLuint texture = GL_NONE;
	    glm::ivec4 viewport {};
	    bool animate = false;
	    /** g_RealTime of the first frame the new wallpaper had content, negative before that */
	    double transitionStart = -1.0;
	};
	static bool allocateHeldFrame (HeldFrame& held);
	void drawTransition (const HeldFrame& held, const glm::ivec4& rect, float progress);
	bool ensureTransitionProgram ();

	Drivers::VideoDriver& m_driver;
	std::map<std::string, std::shared_ptr<CWallpaper>> m_wallpapers = {};
	WallpaperApplication& m_app;
	Media::MediaSource& m_mediaSource;
	std::unique_ptr<TextureCache> m_textureCache = nullptr;
	std::map<std::string, HeldFrame> m_heldFrames = {};
	std::map<std::string, glm::ivec4> m_slotRects = {};
	GLuint m_transitionProgram = GL_NONE;
	bool m_transitionProgramFailed = false;
    };
}
}
