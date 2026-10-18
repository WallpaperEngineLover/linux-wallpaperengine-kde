#include "RenderHarness.h"

using namespace WallpaperEngine::Testing::Harnesses;

const char* argv[] = {
    "",
};

RenderHarness::RenderHarness (std::unique_ptr<ApplicationContext> context, std::unique_ptr<WallpaperApplication> app) :
    m_context (std::move (context)), m_app (std::move (app)), m_driver (*m_context, *m_app) { }

RenderHarness* RenderHarness::build (std::filesystem::path base) {
    auto context = std::make_unique<ApplicationContext> (1, const_cast<char**> (argv));
    auto app = std::make_unique<WallpaperApplication> (*context);

    return new RenderHarness (std::move (context), std::move (app));
}