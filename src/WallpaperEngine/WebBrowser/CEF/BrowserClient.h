#pragma once

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "include/cef_client.h"
#include <atomic>
#include <nlohmann/json.hpp>

namespace WallpaperEngine::WebBrowser::CEF {
nlohmann::json toPageValue (const WallpaperEngine::Data::Model::DynamicValue& property);

class BrowserClient : public CefClient, public CefLifeSpanHandler, public CefDisplayHandler, public CefLoadHandler {
public:
    // properties must outlive this, runWebHost() holds the Project
    BrowserClient (CefRefPtr<CefRenderHandler> ptr, const WallpaperEngine::Data::Model::Properties& properties);

    [[nodiscard]] CefRefPtr<CefRenderHandler> GetRenderHandler () override;
    [[nodiscard]] CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler () override;
    [[nodiscard]] CefRefPtr<CefDisplayHandler> GetDisplayHandler () override;
    [[nodiscard]] CefRefPtr<CefLoadHandler> GetLoadHandler () override;

    // CEF tears the browser down async, our CefRefPtr must not drop it before this fires
    void OnBeforeClose (CefRefPtr<CefBrowser> browser) override;
    [[nodiscard]] bool isClosed () const { return this->m_closed; }

    // forwards the page console, CEF swallows it by default
    bool OnConsoleMessage (
	CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source,
	int line
    ) override;

    void OnLoadError (
	CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText,
	const CefString& failedUrl
    ) override;

    // WE pages expect wallpaperPropertyListener.applyUserProperties once ready, some never render without it
    void OnLoadEnd (CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;

    [[nodiscard]] bool isLoaded () const { return this->m_loaded; }

    CefRefPtr<CefRenderHandler> m_renderHandler = nullptr;

    IMPLEMENT_REFCOUNTING (BrowserClient);

private:
    const WallpaperEngine::Data::Model::Properties& m_properties;
    std::atomic<bool> m_closed = false;
    std::atomic<bool> m_loaded = false;
};
}
