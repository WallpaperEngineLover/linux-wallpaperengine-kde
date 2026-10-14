#pragma once

#include "include/cef_scheme.h"
#include <string>

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser::CEF {
// every web wallpaper under one scheme, resolved by host (workshop id) per request; CEF only takes custom scheme names
// at CefInitialize, so a per-id scheme couldn't cover a hotswapped wallpaper
class WPSchemeHandlerFactory : public CefSchemeHandlerFactory {
public:
    explicit WPSchemeHandlerFactory (const WallpaperEngine::Application::WallpaperApplication& application);

    CefRefPtr<CefResourceHandler> Create (
	CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& scheme_name,
	CefRefPtr<CefRequest> request
    ) override;

private:
    const WallpaperEngine::Application::WallpaperApplication& m_application;

    IMPLEMENT_REFCOUNTING (WPSchemeHandlerFactory);
    DISALLOW_COPY_AND_ASSIGN (WPSchemeHandlerFactory);
};
} // namespace WallpaperEngine::WebBrowser::CEF