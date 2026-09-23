#pragma once

#include "WPSchemeHandlerFactory.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"
#include "include/cef_app.h"

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser::CEF {
class SubprocessApp : public CefApp, public CefRenderProcessHandler {
public:
    explicit SubprocessApp (WallpaperEngine::Application::WallpaperApplication& application);

    void OnRegisterCustomSchemes (CefRawPtr<CefSchemeRegistrar> registrar) override;

    [[nodiscard]] CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler () override;

    void OnContextCreated (
	CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context
    ) override;

protected:
    const WallpaperEngine::Application::WallpaperApplication& getApplication () const;

private:
    WallpaperEngine::Application::WallpaperApplication& m_application;
    IMPLEMENT_REFCOUNTING (SubprocessApp);
    DISALLOW_COPY_AND_ASSIGN (SubprocessApp);
};
}