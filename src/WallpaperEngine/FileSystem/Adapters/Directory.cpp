#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>

#include "Directory.h"

#include "WallpaperEngine/Assets/AssetLoadException.h"

using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::FileSystem::Adapters;

static bool equalsIgnoreCase (const std::string& a, const std::string& b) {
    return a.size () == b.size ()
	&& std::equal (a.begin (), a.end (), b.begin (), [] (unsigned char x, unsigned char y) {
	       return std::tolower (x) == std::tolower (y);
	   });
}

// scenes are authored on Windows, so missing path components fall back to a case-insensitive search
static std::filesystem::path resolveCase (const std::filesystem::path& base, const std::filesystem::path& relative) {
    std::error_code ec;

    if (relative.is_absolute () || std::filesystem::exists (base / relative, ec)) {
	return base / relative;
    }

    std::filesystem::path current = base;

    for (const auto& part : relative) {
	if (std::filesystem::exists (current / part, ec)) {
	    current /= part;
	    continue;
	}

	bool found = false;

	for (const auto& entry : std::filesystem::directory_iterator (current, ec)) {
	    if (equalsIgnoreCase (entry.path ().filename ().string (), part.string ())) {
		current = entry.path ();
		found = true;
		break;
	    }
	}

	if (!found) {
	    return base / relative;
	}
    }

    return current;
}

ReadStreamSharedPtr DirectoryAdapter::open (const std::filesystem::path& path) const {
    auto finalpath = std::filesystem::canonical (resolveCase (this->basepath, path));

    if (finalpath.string ().find (this->basepath.string ()) != 0) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    const auto status = std::filesystem::status (finalpath);

    if (!std::filesystem::exists (finalpath)) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    if (!std::filesystem::is_regular_file (status)) {
	throw std::filesystem::filesystem_error ("Expected file but found a directory", path, std::error_code ());
    }

    return std::make_shared<std::ifstream> (finalpath);
}

bool DirectoryAdapter::exists (const std::filesystem::path& path) const {
    try {
	const auto finalpath = std::filesystem::canonical (resolveCase (this->basepath, path));

	if (finalpath.string ().find (this->basepath.string ()) != 0) {
	    return false;
	}

	const auto status = std::filesystem::status (finalpath);

	if (!std::filesystem::exists (finalpath)) {
	    return false;
	}

	if (!std::filesystem::is_regular_file (status)) {
	    return false;
	}

	return true;
    } catch (std::filesystem::filesystem_error&) {
	return false;
    }
}

std::filesystem::path DirectoryAdapter::physicalPath (const std::filesystem::path& path) const {
    auto finalpath = std::filesystem::canonical (resolveCase (this->basepath, path));

    if (finalpath.string ().find (this->basepath.string ()) != 0) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    return finalpath;
}

std::optional<std::filesystem::path>
DirectoryAdapter::resolveWorkshopDependencyAlias (const std::filesystem::path& path) const {
    const std::filesystem::path dir = path.parent_path ();
    const std::filesystem::path basename = path.filename ();
    const std::filesystem::path workshopDir = this->basepath / dir / "workshop";

    std::optional<std::filesystem::path> found = std::nullopt;

    try {
	if (!std::filesystem::exists (workshopDir) || !std::filesystem::is_directory (workshopDir)) {
	    return std::nullopt;
	}

	for (const auto& entry : std::filesystem::directory_iterator (workshopDir)) {
	    if (!entry.is_directory ()) {
		continue;
	    }

	    const auto candidate = entry.path () / basename;

	    if (!std::filesystem::exists (candidate) || !std::filesystem::is_regular_file (candidate)) {
		continue;
	    }

	    if (found.has_value ()) {
		// more than one dependency ships a same-named file - ambiguous, refuse to guess.
		return std::nullopt;
	    }

	    found = dir / "workshop" / entry.path ().filename () / basename;
	}
    } catch (std::filesystem::filesystem_error&) {
	return std::nullopt;
    }

    return found;
}

bool DirectoryFactory::handlesMountpoint (const std::filesystem::path& path) const {
    try {
	const auto finalpath = std::filesystem::canonical (path);
	const auto status = std::filesystem::status (finalpath);

	return std::filesystem::exists (finalpath) && std::filesystem::is_directory (status);
    } catch (std::filesystem::filesystem_error&) {
	return false;
    }
}

AdapterSharedPtr DirectoryFactory::create (const std::filesystem::path& path) const {
    auto finalpath = std::filesystem::canonical (path);
    const auto status = std::filesystem::status (finalpath);

    if (!std::filesystem::exists (finalpath)) {
	throw std::filesystem::filesystem_error ("Cannot find directory", path, std::error_code ());
    }

    if (!std::filesystem::is_directory (status)) {
	throw std::filesystem::filesystem_error ("Expected directory but found a file", path, std::error_code ());
    }

    return std::make_unique<DirectoryAdapter> (finalpath);
}