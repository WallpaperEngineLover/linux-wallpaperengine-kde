#include "BrowserApp.h"
#include "WPSchemeHandlerFactory.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

using namespace WallpaperEngine::WebBrowser::CEF;

BrowserApp::BrowserApp (WallpaperEngine::Application::WallpaperApplication& application) :
    SubprocessApp (application) { }

CefRefPtr<CefBrowserProcessHandler> BrowserApp::GetBrowserProcessHandler () { return this; }

void BrowserApp::OnContextInitialized () {
    // domain_name = nullptr matches every host under WPENGINE_SCHEME - the factory itself picks
    // the right wallpaper's project per request from the host (workshop id).
    CefRegisterSchemeHandlerFactory (
	WPENGINE_SCHEME, static_cast<const char*> (nullptr), new WPSchemeHandlerFactory (this->getApplication ())
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
    // TODO: activate mock-keychain switch for process_type.empty() if we ever support macOS
}

void BrowserApp::OnBeforeChildProcessLaunch (CefRefPtr<CefCommandLine> command_line) {
    // add back any parameters we had before so the new process can load up everything needed
    for (int i = 1; i < this->getApplication ().getContext ().getArgc (); i++) {
	command_line->AppendArgument (this->getApplication ().getContext ().getArgv ()[i]);
    }

    // The "background id" positional above is only the launch-time value - without this, a
    // subprocess spawned after a hotswap would resolve the wrong (or no) project for its
    // scheme handler lookups.
    const auto& currentBackground = this->getApplication ().getContext ().settings.general.defaultBackground;
    if (!currentBackground.empty ()) {
	command_line->AppendSwitchWithValue ("--current-background", currentBackground.string ());
    }
}