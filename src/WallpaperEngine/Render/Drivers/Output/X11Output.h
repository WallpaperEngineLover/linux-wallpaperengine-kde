#pragma once

#include <chrono>
#include <glm/vec4.hpp>
#include <map>
#include <string>
#include <vector>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include "Output.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"

namespace WallpaperEngine::Render::Drivers::Output {
class X11Output final : public Output {
public:
    X11Output (ApplicationContext& context, VideoDriver& driver);
    ~X11Output () override;

    void reset () override;

    bool renderVFlip () const override;
    bool renderMultiple () const override;
    bool haveImageBuffer () const override;
    void* getImageBuffer () const override;
    uint32_t getImageBufferSize () const override;
    void updateRender () const override;

private:
    void loadScreenInfo ();
    void discoverOutputs (XRRScreenResources* screenResources);
    void validateOutputs () const;
    void initX11Background ();
    void free ();

    /**
     * XFCE, MATE and LXQt hide the root behind an opaque desktop window (xfdesktop, caja, pcmanfm-qt). While one is
     * mapped, frames go into our own window stacked right above it, with an empty input region so clicks still reach
     * theirs. Translucent ones (nemo-desktop) are left alone.
     */
    void updateDesktopWindow () const;
    [[nodiscard]] Window findCoveringDesktopWindow (const std::vector<Window>& stacking) const;
    [[nodiscard]] std::vector<Window> stackingOrder () const;
    [[nodiscard]] bool isDesktopType (Window window) const;
    [[nodiscard]] Window topLevelOf (Window window) const;
    void createDesktopWindow () const;

    Display* m_display = nullptr;
    Pixmap m_pixmap;
    Window m_root;
    GC m_gc;
    char* m_imageData = nullptr;
    uint32_t m_imageSize = 0;
    XImage* m_image = nullptr;
    std::vector<OutputViewport*> m_screens = {};

    mutable Window m_desktopWindow = None;
    mutable bool m_desktopWindowMapped = false;
    mutable std::chrono::steady_clock::time_point m_nextDesktopCheck = {};
};
} // namespace WallpaperEngine::Render::Drivers::Output