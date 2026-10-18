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

    static int runSubprocess (int argc, char* argv[]);

private:
    /** creates the profile folder with a lock file held until this process ends, the mark that it is in use */
    void lockProfile ();

    CefRefPtr<CefApp> m_browserApplication = nullptr;
    CefRefPtr<CefCommandLine> m_commandLine = nullptr;
    std::string m_cachePath;
    int m_profileLock = -1;
    WallpaperEngine::Application::WallpaperApplication& m_wallpaperApplication;
};
}
