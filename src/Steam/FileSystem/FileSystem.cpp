#include "FileSystem.h"
#include "WallpaperEngine/Logging/Log.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

std::vector<std::string> steamRootPaths = {
    ".steam/steam",
    ".local/share/Steam",
    ".var/app/com.valvesoftware.Steam/.local/share/Steam",
    "snap/steam/common/.local/share/Steam",
};

std::filesystem::path detectHomepath () {
    char* home = getenv ("HOME");

    if (home == nullptr) {
	sLog.exception ("Cannot find home directory for the current user");
    }

    const std::filesystem::path path = home;

    if (!std::filesystem::is_directory (path)) {
	sLog.exception ("Cannot find home directory for current user, ", home, " is not a directory");
    }

    return path;
}

namespace {
// the "path" entries of Steam's libraryfolders.vdf, one per library folder on any disk
std::vector<std::filesystem::path> libraryFoldersOf (const std::filesystem::path& steamRoot) {
    static const std::regex entry (R"re("path"\s+"((?:[^"\\]|\\.)*)")re");
    std::vector<std::filesystem::path> folders;

    for (const auto& vdf : { steamRoot / "steamapps/libraryfolders.vdf", steamRoot / "config/libraryfolders.vdf" }) {
	std::ifstream file (vdf);

	if (!file.is_open ()) {
	    continue;
	}

	const std::string text ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());

	for (auto it = std::sregex_iterator (text.begin (), text.end (), entry); it != std::sregex_iterator (); ++it) {
	    const std::string raw = (*it)[1].str ();
	    std::string value;

	    for (size_t i = 0; i < raw.size (); i++) {
		if (raw[i] == '\\' && i + 1 < raw.size ()) {
		    i++;
		}

		value += raw[i];
	    }

	    if (!value.empty ()) {
		folders.emplace_back (value);
	    }
	}
    }

    return folders;
}

// every Steam install and the library folders they list, each once, in lookup order
std::vector<std::filesystem::path> steamLibraries () {
    const auto home = detectHomepath ();
    std::vector<std::filesystem::path> libraries;
    std::error_code error;

    const auto add = [&] (const std::filesystem::path& folder) {
	if (!std::filesystem::is_directory (folder, error)) {
	    return;
	}

	const auto canonical = std::filesystem::weakly_canonical (folder, error);

	if (std::ranges::find (libraries, canonical) == libraries.end ()) {
	    libraries.push_back (canonical);
	}
    };

    for (const auto& root : steamRootPaths) {
	add (home / root);
    }

    const auto installs = libraries;

    for (const auto& install : installs) {
	for (const auto& folder : libraryFoldersOf (install)) {
	    add (folder);
	}
    }

    return libraries;
}
} // namespace

std::filesystem::path Steam::FileSystem::workshopDirectory (int appID, const std::string& contentID) {
    for (const auto& library : steamLibraries ()) {
	auto currentpath = library / "steamapps/workshop/content" / std::to_string (appID) / contentID;

	if (!std::filesystem::exists (currentpath) || !std::filesystem::is_directory (currentpath)) {
	    continue;
	}

	return currentpath;
    }

    sLog.exception ("Cannot find workshop directory for steam app ", appID, " and content ", contentID);
}

std::filesystem::path Steam::FileSystem::appDirectory (const std::string& appDirectory, const std::string& path) {
    for (const auto& library : steamLibraries ()) {
	auto currentpath = library / "steamapps/common" / appDirectory / path;

	if (!std::filesystem::exists (currentpath) || !std::filesystem::is_directory (currentpath)) {
	    continue;
	}

	return currentpath;
    }

    sLog.exception ("Cannot find directory for steam app ", appDirectory, ": ", path);
}
