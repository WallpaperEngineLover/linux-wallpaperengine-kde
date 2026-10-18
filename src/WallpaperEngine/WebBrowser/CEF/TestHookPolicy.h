#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace WallpaperEngine::WebBrowser::CEF {
/** Chromium switches the LWE_CEF_SWITCHES debugging variable may not set: they turn the sandbox off, open a debugging
 * port or make Chromium run a program */
inline bool weakensWebSandbox (std::string_view option) {
    constexpr std::array<std::string_view, 17> refused = {
	"no-sandbox",
	"disable-gpu-sandbox",
	"disable-setuid-sandbox",
	"disable-seccomp-filter-sandbox",
	"disable-namespace-sandbox",
	"allow-sandbox-debugging",
	"remote-debugging-port",
	"remote-debugging-pipe",
	"remote-debugging-address",
	"load-extension",
	"renderer-cmd-prefix",
	"utility-cmd-prefix",
	"gpu-launcher",
	"ppapi-plugin-launcher",
	"browser-subprocess-path",
	"js-flags",
	"single-process",
    };

    while (option.starts_with ('-')) {
	option.remove_prefix (1);
    }

    option = option.substr (0, option.find ('='));

    return std::ranges::find (refused, option) != refused.end ();
}

/** A library LWE_WEB_HOST_PRELOAD may put in front of the host's own: absolute, a regular file of ours that no one else
 * can write */
inline bool trustedPreloadFile (const std::string& path) {
    struct stat info {};

    return path.starts_with ('/') && stat (path.c_str (), &info) == 0 && S_ISREG (info.st_mode)
	&& info.st_uid == geteuid () && (info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}
} // namespace WallpaperEngine::WebBrowser::CEF
