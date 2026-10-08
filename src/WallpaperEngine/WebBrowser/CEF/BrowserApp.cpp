#include "BrowserApp.h"
#include "WPSchemeHandlerFactory.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

#include <cstdlib>
#include <sstream>
#include <string>

using namespace WallpaperEngine::WebBrowser::CEF;

BrowserApp::BrowserApp (WallpaperEngine::Application::WallpaperApplication& application) :
    m_application (application) { }

CefRefPtr<CefBrowserProcessHandler> BrowserApp::GetBrowserProcessHandler () { return this; }

void BrowserApp::OnContextInitialized () {
    // domain_name = nullptr matches every host under WPENGINE_SCHEME - the factory itself picks
    // the right wallpaper's project per request from the host (workshop id).
    CefRegisterSchemeHandlerFactory (
	WPENGINE_SCHEME, static_cast<const char*> (nullptr), new WPSchemeHandlerFactory (this->m_application)
    );
}

void BrowserApp::OnBeforeCommandLineProcessing (const CefString& process_type, CefRefPtr<CefCommandLine> command_line) {
    command_line->AppendSwitchWithValue (
	"--disable-features",
	"IsolateOrigins,HardwareMediaKeyHandling,WebContentsOcclusion,RendererCodeIntegrityEnabled,site-per-process"
    );
    command_line->AppendSwitch ("--disable-gpu-shader-disk-cache");
    // no keyring is ever needed, and without this every process tries (and fails) to reach kwalletd
    command_line->AppendSwitchWithValue ("--password-store", "basic");
    command_line->AppendSwitch ("--disable-site-isolation-trials");
    command_line->AppendSwitch ("--disable-web-security");
    command_line->AppendSwitchWithValue ("--remote-allow-origins", "*");
    command_line->AppendSwitchWithValue ("--autoplay-policy", "no-user-gesture-required");
    command_line->AppendSwitch ("--disable-background-timer-throttling");
    command_line->AppendSwitch ("--disable-backgrounding-occluded-windows");
    command_line->AppendSwitch ("--disable-background-media-suspend");
    command_line->AppendSwitch ("--disable-renderer-backgrounding");
    command_line->AppendSwitch ("--disable-test-root-certs");
    command_line->AppendSwitch ("--disable-bundled-ppapi-flash");
    command_line->AppendSwitch ("--disable-breakpad");
    command_line->AppendSwitch ("--disable-field-trial-config");
    command_line->AppendSwitch ("--no-experiments");

    // debugging: extra Chromium switches, e.g. swiftshader where the GPU process can't start
    if (const char* extra = std::getenv ("LWE_CEF_SWITCHES"); extra != nullptr) {
	std::istringstream stream (extra);
	std::string word;

	while (stream >> word) {
	    if (const auto equals = word.find ('='); equals != std::string::npos) {
		command_line->AppendSwitchWithValue (word.substr (0, equals), word.substr (equals + 1));
	    } else {
		command_line->AppendSwitch (word);
	    }
	}
    }
}
