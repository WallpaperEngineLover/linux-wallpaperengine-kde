#include "SubprocessApp.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

using namespace WallpaperEngine::WebBrowser::CEF;

SubprocessApp::SubprocessApp (WallpaperEngine::Application::WallpaperApplication& application) :
    m_application (application) { }

void SubprocessApp::OnRegisterCustomSchemes (CefRawPtr<CefSchemeRegistrar> registrar) {
    // One fixed scheme shared by every web wallpaper; the factory resolves the project per
    // request by host (workshop id).
    registrar->AddCustomScheme (
	WPENGINE_SCHEME, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_FETCH_ENABLED
    );
}

const WallpaperEngine::Application::WallpaperApplication& SubprocessApp::getApplication () const {
    return this->m_application;
}