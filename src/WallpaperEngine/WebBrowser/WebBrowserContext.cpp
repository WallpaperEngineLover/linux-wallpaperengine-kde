#include "WebBrowserContext.h"
#include "CEF/BrowserApp.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Desktop/SessionBus.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/WebBrowser/CEF/SubprocessApp.h"
#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"
#include "include/cef_app.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <random>
#include <sched.h>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace WallpaperEngine::WebBrowser;

namespace uuid {
static std::random_device rd;
static std::mt19937 gen (rd ());
static std::uniform_int_distribution<> dis (0, 15);
static std::uniform_int_distribution<> dis2 (8, 11);

static std::string generate_uuid_v4 () {
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
}

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
    // a later engine run removes it if we get killed before cleaning up, unless something still holds its lock
    this->m_cachePath = (std::filesystem::temp_directory_path ()
			 / ("lwe-cef-" + Desktop::processTag () + "-" + uuid::generate_uuid_v4 ()))
			    .string ();
    this->lockProfile ();
    cef_string_utf8_to_utf16 (this->m_cachePath.c_str (), this->m_cachePath.length (), &settings.root_cache_path);
    settings.windowless_rendering_enabled = true;

    if (const std::string helper = subprocessHelper ().string (); !helper.empty ()) {
	cef_string_utf8_to_utf16 (helper.c_str (), helper.length (), &settings.browser_subprocess_path);
    } else {
	sLog.out ("linux-wallpaperengine-web-helper not found next to the engine, Chromium's processes start slower");
    }

    // Chromium's own ERROR-level chatter (cancelled requests and the like) is noise here, the page's console is
    // still forwarded by BrowserClient
    settings.log_severity = std::getenv ("LWE_CEF_LOG") != nullptr ? LOGSEVERITY_VERBOSE : LOGSEVERITY_FATAL;
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

    if (this->m_profileLock >= 0) {
	close (this->m_profileLock);
    }
}

void WebBrowserContext::lockProfile () {
    std::error_code error;
    std::filesystem::create_directories (this->m_cachePath, error);

    const auto lockFile = std::filesystem::path (this->m_cachePath) / IPC::CEF_PROFILE_LOCK;
    this->m_profileLock = open (lockFile.c_str (), O_CREAT | O_RDWR | O_CLOEXEC, 0600);

    if (this->m_profileLock >= 0 && flock (this->m_profileLock, LOCK_SH) != 0) {
	close (this->m_profileLock);
	this->m_profileLock = -1;
    }
}
