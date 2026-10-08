#pragma once

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/WebBrowser/Scheme.h"
#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"
#include "include/wrapper/cef_helpers.h"

#include <string>

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser::CEF {
class BrowserApp;
}

namespace WallpaperEngine::WebBrowser {
class WebBrowserContext {
public:
    explicit WebBrowserContext (WallpaperEngine::Application::WallpaperApplication& wallpaperApplication);
    ~WebBrowserContext ();

    /** Chromium child processes re-exec'd as this binary when the web helper is missing, skips engine setup */
    static int runSubprocess (int argc, char* argv[]);

private:
    CefRefPtr<CefApp> m_browserApplication = nullptr;
    CefRefPtr<CefCommandLine> m_commandLine = nullptr;
    // Chromium profile of this process, created under the temp directory and removed once CEF is shut down
    std::string m_cachePath;
    WallpaperEngine::Application::WallpaperApplication& m_wallpaperApplication;
};
} // namespace WallpaperEngine::WebBrowser
