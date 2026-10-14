#pragma once

#include "WallpaperEngine/Testing/Render/TestingOpenGLDriver.h"

namespace WallpaperEngine::Testing::Harnesses {
using namespace WallpaperEngine::Testing::Render;
// replaces some gl* calls with tracking shims, careful with gl* calls in tests
class RenderHarness {
public:
    static RenderHarness* build (std::filesystem::path base);

    ~RenderHarness ();

protected:
    RenderHarness (ApplicationContext* context, WallpaperApplication* app);

private:
    TestingOpenGLDriver m_driver;
    ApplicationContext* m_context;
    WallpaperApplication* m_app;
};
} // namespace WallpaperEngine::Testing::Harnesses
