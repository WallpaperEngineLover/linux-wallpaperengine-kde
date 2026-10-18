#pragma once

#include "Output.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"

namespace WallpaperEngine::Render::Drivers::Output {
class GLFWWindowOutput final : public Output {
public:
    GLFWWindowOutput (ApplicationContext& context, VideoDriver& driver);
    ~GLFWWindowOutput () override;

    void reset () override;
    bool renderVFlip () const override;
    bool renderMultiple () const override;
    bool haveImageBuffer () const override;
    void* getImageBuffer () const override;
    uint32_t getImageBufferSize () const override;
    void updateRender () const override;

private:
    void repositionWindow () const;
    void createBackgroundWindow ();
    void destroyBackgroundWindow ();
    void keepBackgroundWindowLow () const;
    bool desktopWindowBelowUs () const;

    // --window-background: the frames go into a plain X window of our own, see createBackgroundWindow
    // Xlib types are kept out of this header, its macros clash with the rest of the engine
    void* m_display = nullptr;
    unsigned long m_background = 0;
    void* m_gc = nullptr;
    void* m_image = nullptr;
    char* m_imageData = nullptr;
    uint32_t m_imageSize = 0;
    mutable uint32_t m_updates = 0;
    mutable unsigned long m_nudge = 0;
    mutable uint32_t m_nudgeUntil = 0;
};
}