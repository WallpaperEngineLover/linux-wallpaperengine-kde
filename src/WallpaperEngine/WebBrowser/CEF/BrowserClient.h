#pragma once

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "include/cef_client.h"
#include <atomic>

namespace WallpaperEngine::WebBrowser::CEF {
// Provides access to browser-instance-specific callbacks. A single CefClient instance can be
// shared among any number of browsers.
class BrowserClient : public CefClient, public CefLifeSpanHandler, public CefDisplayHandler, public CefLoadHandler {
public:
    // properties must outlive this instance - runWebHost() holds the owning Project for as long as
    // the browser (and this client) is alive.
    BrowserClient (CefRefPtr<CefRenderHandler> ptr, const WallpaperEngine::Data::Model::Properties& properties);

    [[nodiscard]] CefRefPtr<CefRenderHandler> GetRenderHandler () override;
    [[nodiscard]] CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler () override;
    [[nodiscard]] CefRefPtr<CefDisplayHandler> GetDisplayHandler () override;
    [[nodiscard]] CefRefPtr<CefLoadHandler> GetLoadHandler () override;

    // CEF owns the browser's actual teardown asynchronously - our own CefRefPtr<CefBrowser> must
    // not be allowed to drop to zero (destroying it) before this fires, or ~CWeb crashes tearing
    // down a browser CEF itself isn't done with yet.
    void OnBeforeClose (CefRefPtr<CefBrowser> browser) override;
    [[nodiscard]] bool isClosed () const { return this->m_closed; }

    // Surfaces the page's own console (JS errors/warnings, WebGL failures, etc) - CEF swallows
    // this by default, and it's often the only clue a page is failing silently.
    bool OnConsoleMessage (
	CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source,
	int line
    ) override;

    void OnLoadError (
	CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText,
	const CefString& failedUrl
    ) override;

    // WE web wallpapers expect the host to call
    // window.wallpaperPropertyListener.applyUserProperties(...) once the page is ready - some
    // gate their entire render loop on a property only ever set inside that callback, so skipping
    // this leaves them stuck rendering nothing, silently, forever.
    void OnLoadEnd (CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;

    CefRefPtr<CefRenderHandler> m_renderHandler = nullptr;

    IMPLEMENT_REFCOUNTING (BrowserClient);

private:
    const WallpaperEngine::Data::Model::Properties& m_properties;
    std::atomic<bool> m_closed = false;
};
} // namespace WallpaperEngine::WebBrowser::CEF
