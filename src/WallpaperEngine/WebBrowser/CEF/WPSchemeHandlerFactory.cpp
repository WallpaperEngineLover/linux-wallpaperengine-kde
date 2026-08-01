#include "WPSchemeHandlerFactory.h"
#include "WPSchemeHandler.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_helpers.h"
#include <iostream>
#include <ranges>

using namespace WallpaperEngine::WebBrowser::CEF;

WPSchemeHandlerFactory::WPSchemeHandlerFactory (const WallpaperEngine::Application::WallpaperApplication& application) :
    m_application (application) { }

CefRefPtr<CefResourceHandler> WPSchemeHandlerFactory::Create (
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& scheme_name,
    CefRefPtr<CefRequest> request
) {
    CEF_REQUIRE_IO_THREAD ();

    CefURLParts parts;

    if (!CefParseURL (request->GetURL (), parts)) {
	return nullptr;
    }

    // Hosts are "w<workshopId>" (see CWeb.cpp) - the leading letter keeps the host from being
    // purely numeric, since Chromium's URL canonicalizer treats an all-digit host as a legacy
    // decimal-form IPv4 address and silently rewrites it, which would otherwise break every
    // workshopId == host comparison below.
    const std::string host = CefString (&parts.host);

    if (host.size () < 2 || host[0] != 'w') {
	return nullptr;
    }

    const std::string workshopId = host.substr (1);

    for (const auto& info : this->m_application.getBackgrounds () | std::views::values) {
	if (info->workshopId == workshopId) {
	    return new WPSchemeHandler (*info);
	}
    }

    std::cout << "WPSchemeHandlerFactory: no loaded project for workshop id " << workshopId
	       << " (request: " << request->GetURL ().ToString () << ")" << std::endl;

    return nullptr;
}