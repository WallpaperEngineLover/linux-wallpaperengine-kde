#include "BrowserClient.h"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>

using namespace WallpaperEngine::WebBrowser::CEF;
using namespace WallpaperEngine::Data::Model;

namespace {
// Wallpaper Engine hands web pages colors and vectors as "x y z" and combo values as numbers
// ("value == 1" and switch/case both show up in wallpapers, and the latter is strict)
nlohmann::json toPageValue (const DynamicValue& property) {
    std::ostringstream ss;

    switch (property.getType ()) {
	case DynamicValue::Boolean: return property.getBool ();
	case DynamicValue::Int: return property.getInt ();
	case DynamicValue::Float: return property.getFloat ();
	case DynamicValue::Vec2: ss << property.getVec2 ().x << " " << property.getVec2 ().y; return ss.str ();
	case DynamicValue::Vec3:
	case DynamicValue::Vec4:
	    ss << property.getVec3 ().x << " " << property.getVec3 ().y << " " << property.getVec3 ().z;
	    return ss.str ();
	case DynamicValue::String: {
	    const auto& text = property.getString ();

	    if (dynamic_cast<const PropertyCombo*> (&property) == nullptr) {
		return text;
	    }

	    int asInt = 0;
	    const auto end = text.data () + text.size ();

	    if (!text.empty () && std::from_chars (text.data (), end, asInt).ptr == end) {
		return asInt;
	    }

	    char* parsedEnd = nullptr;
	    const double asNumber = std::strtod (text.c_str (), &parsedEnd);

	    if (!text.empty () && parsedEnd == text.c_str () + text.size ()) {
		return asNumber;
	    }

	    return text;
	}
	default: return property.toString ();
    }
}
} // namespace

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
    // some wallpapers log continuously, below warnings only the first few messages are forwarded
    static std::atomic<int> chatterBudget = 60;

    if (level < LOGSEVERITY_WARNING && chatterBudget.fetch_sub (1) <= 0) {
	return false;
    }

    // A page that trips over the same error every animation frame (WebGL: INVALID_ENUM from a legacy particle
    // script) would otherwise write hundreds of identical lines a second: show the first few, then every tenfold
    static std::mutex repeatLock;
    static std::unordered_map<std::string, unsigned> repeats;

    const std::string text = message.ToString ();
    unsigned seen = 1;

    {
	std::lock_guard guard (repeatLock);

	if (repeats.size () < 4096 || repeats.contains (text)) {
	    seen = ++repeats[text];
	}
    }

    bool show = seen <= 3;

    for (unsigned step = 10; !show && step <= seen; step *= 10) {
	show = seen == step;
    }

    if (!show) {
	return false;
    }

    std::cout << "[web console] " << source.ToString () << ":" << line << ": " << text;

    if (seen > 3) {
	std::cout << " (seen " << seen << " times)";
    }

    std::cout << std::endl;
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
	const auto value = toPageValue (*property);

	props[name] = { { "value", value } };
    }

    // through the page shim (see SubprocessApp) when there is one, it holds on to them for pages whose listener isn't
    // defined yet
    const std::string script = "if (window.__lweProperties) { window.__lweProperties(" + props.dump ()
	+ "); } else if (window.wallpaperPropertyListener && window.wallpaperPropertyListener.applyUserProperties) { "
	  "window.wallpaperPropertyListener.applyUserProperties(" + props.dump () + "); }";

    frame->ExecuteJavaScript (script, frame->GetURL (), 0);
    this->m_loaded = true;
}
