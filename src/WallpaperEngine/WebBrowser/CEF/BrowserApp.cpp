#include "BrowserApp.h"
#include "TestHookPolicy.h"
#include "WPSchemeHandlerFactory.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/WebBrowser/WebBrowserContext.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>

using namespace WallpaperEngine::WebBrowser::CEF;

BrowserApp::BrowserApp (WallpaperEngine::Application::WallpaperApplication& application) :
    m_application (application) { }

CefRefPtr<CefBrowserProcessHandler> BrowserApp::GetBrowserProcessHandler () { return this; }

void BrowserApp::OnContextInitialized () {
    // nullptr domain matches every host, the factory picks the project by workshop id
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

    const char* extra = std::getenv ("LWE_CEF_SWITCHES");

    // Chromium starts its X11 platform even for windowless browsers and exits without an X server: a Flatpak on
    // Wayland (fallback-x11 hides X when Wayland is there) or a session without Xwayland. The headless platform with
    // ANGLE on EGL renders the same, WebGL on the GPU included
    const char* display = std::getenv ("DISPLAY");
    const bool dmabufFrames = m_application.getContext ().settings.general.webHostFrames == "dmabuf";

    if (dmabufFrames
	&& (extra == nullptr || std::string_view (extra).find ("--ozone-platform") == std::string_view::npos)) {
	// headless makes no GPU buffers one can share ("unable to allocate frame"), Wayland's come from the
	// compositor's dmabuf support and X11's need DRI3 on the server
	const char* wayland = std::getenv ("WAYLAND_DISPLAY");
	command_line->AppendSwitchWithValue (
	    "--ozone-platform", wayland != nullptr && wayland[0] != '\0' ? "wayland" : "x11"
	);
	command_line->AppendSwitchWithValue ("--use-gl", "angle");
	command_line->AppendSwitchWithValue ("--use-angle", "gl-egl");
    } else if (
	(display == nullptr || *display == '\0')
	&& (extra == nullptr || std::string_view (extra).find ("--ozone-platform") == std::string_view::npos)
    ) {
	command_line->AppendSwitchWithValue ("--ozone-platform", "headless");
	command_line->AppendSwitchWithValue ("--use-gl", "angle");
	command_line->AppendSwitchWithValue ("--use-angle", "gl-egl");
    }

    // Chromium 146's GPU process loses its context on the first decoded video frame under the headless platform
    // ("Could not find SharedImageBackingFactory ... BGRA_8888", then a restart loop and a page that never paints, seen
    // with 2681203626's webm), software decoding is fine
    const bool headlessPlatform
	= (!dmabufFrames && (display == nullptr || *display == '\0')
	   && (extra == nullptr || std::string_view (extra).find ("--ozone-platform") == std::string_view::npos))
	|| (extra != nullptr && std::string_view (extra).find ("--ozone-platform=headless") != std::string_view::npos);

    if (headlessPlatform) {
	command_line->AppendSwitch ("--disable-accelerated-video-decode");
    }

    // debugging: extra Chromium switches, e.g. swiftshader where the GPU process can't start
    if (extra != nullptr) {
	std::istringstream stream (extra);
	std::string word;

	while (stream >> word) {
	    if (weakensWebSandbox (word)) {
		sLog.error ("LWE_CEF_SWITCHES: ignoring ", word, ", it would weaken the web sandbox");
		continue;
	    }

	    if (const auto equals = word.find ('='); equals != std::string::npos) {
		command_line->AppendSwitchWithValue (word.substr (0, equals), word.substr (equals + 1));
	    } else {
		command_line->AppendSwitch (word);
	    }
	}
    }
}
