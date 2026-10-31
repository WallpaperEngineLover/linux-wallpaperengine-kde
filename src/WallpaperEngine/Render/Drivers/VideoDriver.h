#pragma once

#include "WallpaperEngine/Input/InputContext.h"
#include "WallpaperEngine/Input/MouseInput.h"
#include "WallpaperEngine/Render/Drivers/Output/Output.h"
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <string>

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::Input {
class InputContext;
class CWaylandMouseInput;
}

namespace WallpaperEngine::Render::Drivers {
namespace Detectors {
    class FullScreenDetector;
}

class VideoDriver {
public:
    class SharedContext {
    public:
	virtual ~SharedContext () = default;
	virtual bool makeCurrent () = 0;
	virtual void release () = 0;
    };

    explicit VideoDriver (WallpaperApplication& app, Input::MouseInput& mouseInput);
    virtual ~VideoDriver () = default;

    [[nodiscard]] virtual Output::Output& getOutput () = 0;
    [[nodiscard]] virtual float getRenderTime () const = 0;
    virtual bool closeRequested () = 0;
    virtual void resizeWindow (glm::ivec2 size) = 0;
    virtual void resizeWindow (glm::ivec4 positionAndSize) = 0;
    virtual void showWindow () = 0;
    virtual void hideWindow () = 0;
    [[nodiscard]] virtual glm::ivec2 getFramebufferSize () const = 0;
    [[nodiscard]] virtual uint32_t getFrameCounter () const = 0;
    [[nodiscard]] virtual void* getProcAddress (const char* name) const = 0;
    [[nodiscard]] virtual void* getWaylandDisplay () const { return nullptr; }
    [[nodiscard]] virtual void* getX11Display () const { return nullptr; }
    // --hdr and a compositor that takes PQ: videos decode to linear BT.2020, HDR outputs get PQ
    [[nodiscard]] virtual bool isHDRAvailable () const { return false; }
    [[nodiscard]] virtual std::unique_ptr<SharedContext> createSharedContext () const { return nullptr; }
    virtual void dispatchEventQueue () = 0;
    virtual void dispatchWhilePaused () { }
    [[nodiscard]] WallpaperApplication& getApp () const;
    [[nodiscard]] Input::InputContext& getInputContext ();

private:
    WallpaperApplication& m_app;
    Input::InputContext m_inputContext;
};
}