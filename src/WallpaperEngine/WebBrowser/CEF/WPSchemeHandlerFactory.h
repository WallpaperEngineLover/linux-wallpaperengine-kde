#pragma once

#include "include/cef_scheme.h"
#include <string>

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser::CEF {
/**
 * Serves every web wallpaper's files under one fixed scheme (WPENGINE_SCHEME), resolving which
 * wallpaper a request belongs to from its URL host (the workshop id) at request time. This has
 * to stay dynamic rather than bound to one wallpaper at construction: CEF only allows declaring
 * valid custom scheme names once, very early during CefInitialize, so a scheme registered for one
 * workshop id could never cover a different web wallpaper hotswapped in later in the same
 * process - hence one shared scheme with a runtime lookup instead of one scheme per id.
 */
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