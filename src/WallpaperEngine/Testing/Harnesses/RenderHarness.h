#pragma once

#include <memory>

#include "WallpaperEngine/Testing/Render/TestingOpenGLDriver.h"

namespace WallpaperEngine::Testing::Harnesses {
using namespace WallpaperEngine::Testing::Render;
// replaces some gl* calls with tracking shims, careful with gl* calls in tests
class RenderHarness {
public:
    static RenderHarness* build (std::filesystem::path base);

protected:
    RenderHarness (std::unique_ptr<ApplicationContext> context, std::unique_ptr<WallpaperApplication> app);

private:
    // the driver refers to the other two, it has to go first
    std::unique_ptr<ApplicationContext> m_context;
    std::unique_ptr<WallpaperApplication> m_app;
    TestingOpenGLDriver m_driver;
};
}
