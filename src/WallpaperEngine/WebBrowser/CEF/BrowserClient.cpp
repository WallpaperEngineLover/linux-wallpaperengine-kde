#include "BrowserClient.h"
#include <iostream>
#include <nlohmann/json.hpp>

using namespace WallpaperEngine::WebBrowser::CEF;
using namespace WallpaperEngine::Data::Model;

BrowserClient::BrowserClient (CefRefPtr<CefRenderHandler> ptr, const Properties& properties) :
    m_renderHandler (std::move (ptr)), m_properties (properties) { }

CefRefPtr<CefRenderHandler> BrowserClient::GetRenderHandler () { return m_renderHandler; }

CefRefPtr<CefLifeSpanHandler> BrowserClient::GetLifeSpanHandler () { return this; }

CefRefPtr<CefDisplayHandler> BrowserClient::GetDisplayHandler () { return this; }

CefRefPtr<CefLoadHandler> BrowserClient::GetLoadHandler () { return this; }

void BrowserClient::OnBeforeClose (CefRefPtr<CefBrowser> browser) { this->m_closed = true; }

bool BrowserClient::OnConsoleMessage (
    CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source,
    int line
) {
    // Some wallpapers log continuously during normal operation (Live2D motion transitions, asset
    // fetch tracing, etc via console.log/INFO) - forwarding every one of those means a synchronous
    // flushed write for the lifetime of the wallpaper, across two processes on a dual-monitor setup.
    // Only warnings and above are worth the cost; they're also the only ones actually useful for
    // spotting a page failing silently.
    if (level < LOGSEVERITY_WARNING) {
	return false;
    }

    std::cout << "[web console] " << source.ToString () << ":" << line << ": " << message.ToString () << std::endl;
    return false;
}

void BrowserClient::OnLoadError (
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText,
    const CefString& failedUrl
) {
    std::cout << "[web load error] " << failedUrl.ToString () << ": " << errorText.ToString () << " (" << errorCode
	       << ")" << std::endl;
}

void BrowserClient::OnLoadEnd (CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) {
    if (!frame->IsMain ()) {
	return;
    }

    nlohmann::json props = nlohmann::json::object ();

    for (const auto& [name, property] : this->m_properties) {
	nlohmann::json value;

	switch (property->getType ()) {
	case DynamicValue::Boolean: value = property->getBool (); break;
	case DynamicValue::Int: value = property->getInt (); break;
	case DynamicValue::Float: value = property->getFloat (); break;
	case DynamicValue::String: value = property->getString (); break;
	default: value = property->toString (); break;
	}

	props[name] = { { "value", value } };
    }

    const std::string script = "if (window.wallpaperPropertyListener && "
				"window.wallpaperPropertyListener.applyUserProperties) { "
				"window.wallpaperPropertyListener.applyUserProperties(" + props.dump ()
	+ "); }";

    frame->ExecuteJavaScript (script, frame->GetURL (), 0);
}
