#pragma once

#include <memory>

#include "Output.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"
#include "X11BackgroundWindow.h"

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
    [[nodiscard]] bool hasBackground () const;

    std::unique_ptr<X11BackgroundWindow> m_background;
};
}
