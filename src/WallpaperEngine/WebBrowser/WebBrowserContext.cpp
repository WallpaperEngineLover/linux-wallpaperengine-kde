#include "WebBrowserContext.h"
#include "CEF/BrowserApp.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/WebBrowser/CEF/SubprocessApp.h"
#include "include/cef_app.h"
#include "include/cef_render_handler.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <random>
#include <sched.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace WallpaperEngine::WebBrowser;

// TODO: THIS IS USED TO GENERATE A RANDOM FOLDER FOR THE CHROME PROFILE, MAYBE A DIFFERENT APPROACH WOULD BE BETTER?
namespace uuid {
static std::random_device rd;
static std::mt19937 gen (rd ());
static std::uniform_int_distribution<> dis (0, 15);
static std::uniform_int_distribution<> dis2 (8, 11);

std::string generate_uuid_v4 () {
    std::stringstream ss;
    int i;
    ss << std::hex;
    for (i = 0; i < 8; i++) {
	ss << dis (gen);
    }
    ss << "-";
    for (i = 0; i < 4; i++) {
	ss << dis (gen);
    }
    ss << "-4";
    for (i = 0; i < 3; i++) {
	ss << dis (gen);
    }
    ss << "-";
    ss << dis2 (gen);
    for (i = 0; i < 3; i++) {
	ss << dis (gen);
    }
    ss << "-";
    for (i = 0; i < 12; i++) {
	ss << dis (gen);
    };
    return ss.str ();
}
}

namespace {
// empty when missing, Chromium then re-execs this binary (main() hands it to runSubprocess)
std::filesystem::path subprocessHelper () {
    constexpr std::string_view deletedSuffix = " (deleted)";
    std::error_code error;
    std::string self = std::filesystem::read_symlink ("/proc/self/exe", error).string ();

    if (error) {
	return {};
    }

    if (self.ends_with (deletedSuffix)) {
	self.resize (self.size () - deletedSuffix.size ());
    }

    auto helper = std::filesystem::path (self).parent_path () / "linux-wallpaperengine-web-helper";

    if (access (helper.c_str (), X_OK) != 0) {
	return {};
    }

    return helper;
}

bool writeFile (const char* path, const char* text, size_t length) {
    const int fd = open (path, O_WRONLY);

    if (fd < 0) {
	return false;
    }

    const bool written = write (fd, text, length) == static_cast<ssize_t> (length);
    close (fd);
    return written;
}

// Ubuntu 24.04+ allows the unshare but AppArmor drops its capabilities, so the uid map write is what fails
bool userNamespacesWork () {
    char uidMap[64];
    const int uidMapLength = snprintf (uidMap, sizeof (uidMap), "0 %u 1\n", getuid ());
    const pid_t child = fork ();

    if (child < 0) {
	return false;
    }

    if (child == 0) {
	const bool mapped = unshare (CLONE_NEWUSER) == 0 && writeFile ("/proc/self/setgroups", "deny", 4)
	    && writeFile ("/proc/self/uid_map", uidMap, uidMapLength);
	_exit (mapped ? 0 : 1);
    }

    int status = 0;

    while (waitpid (child, &status, 0) < 0 && errno == EINTR) { }

    return WIFEXITED (status) && WEXITSTATUS (status) == 0;
}

// Chromium aborts without a user namespace or a setuid root chrome-sandbox next to libcef
bool chromiumSandboxUsable () {
    std::error_code error;
    const auto self = std::filesystem::read_symlink ("/proc/self/exe", error);
    struct stat sandbox {};

    if (!error && stat ((self.parent_path () / "chrome-sandbox").c_str (), &sandbox) == 0 && sandbox.st_uid == 0
	&& (sandbox.st_mode & S_ISUID) != 0) {
	return true;
    }

    return userNamespacesWork ();
}
} // namespace

int WebBrowserContext::runSubprocess (int argc, char* argv[]) {
    const CefMainArgs mainArgs (argc, argv);

    return CefExecuteProcess (mainArgs, new CEF::SubprocessApp (), nullptr);
}

WebBrowserContext::WebBrowserContext (WallpaperEngine::Application::WallpaperApplication& wallpaperApplication) :
    m_browserApplication (nullptr), m_wallpaperApplication (wallpaperApplication) {
    CefMainArgs main_args (
	this->m_wallpaperApplication.getContext ().getArgc (), this->m_wallpaperApplication.getContext ().getArgv ()
    );

    this->m_browserApplication = new CEF::BrowserApp (wallpaperApplication);

    CefSettings settings;
    // the pid in the name lets a later engine run tell this profile is stale if we get killed before cleaning up
    this->m_cachePath = (std::filesystem::temp_directory_path ()
			 / ("lwe-cef-" + std::to_string (getpid ()) + "-" + uuid::generate_uuid_v4 ()))
			    .string ();
    cef_string_utf8_to_utf16 (this->m_cachePath.c_str (), this->m_cachePath.length (), &settings.root_cache_path);
    settings.windowless_rendering_enabled = true;

    if (const std::string helper = subprocessHelper ().string (); !helper.empty ()) {
	cef_string_utf8_to_utf16 (helper.c_str (), helper.length (), &settings.browser_subprocess_path);
    } else {
	sLog.out ("linux-wallpaperengine-web-helper not found next to the engine, Chromium's processes start slower");
    }

    // Chromium's own ERROR-level chatter (cancelled requests and the like) is noise here, the page's console is
    // still forwarded by BrowserClient
    settings.log_severity = LOGSEVERITY_FATAL;
#if defined(CEF_NO_SANDBOX)
    settings.no_sandbox = true;
#endif

    // tests only: containers without a setuid chrome-sandbox or user namespaces
    if (const char* noSandbox = std::getenv ("LWE_CEF_NO_SANDBOX"); noSandbox != nullptr && noSandbox[0] == '1') {
	sLog.error ("LWE_CEF_NO_SANDBOX is set, web content runs without Chromium's sandbox");
	settings.no_sandbox = true;
    } else if (!settings.no_sandbox && !chromiumSandboxUsable ()) {
	sLog.error (
	    "No user namespaces and no setuid root chrome-sandbox, web content runs without Chromium's sandbox. "
	    "Distribution packages set it up, or as root: chown root:root chrome-sandbox; chmod 4755 chrome-sandbox"
	);
	settings.no_sandbox = true;
    }

    if (!CefInitialize (main_args, settings, this->m_browserApplication, nullptr)) {
	sLog.exception ("CefInitialize: failed");
    }
}

WebBrowserContext::~WebBrowserContext () {
    sLog.out ("Shutting down CEF");
    CefShutdown ();

    if (!this->m_cachePath.empty ()) {
	std::error_code error;
	std::filesystem::remove_all (this->m_cachePath, error);
    }
}
