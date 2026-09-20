#include <fstream>
#include <memory>

#include "Package.h"

#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Data/Parsers/PackageParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Data/Utils/MemoryStream.h"

#include <algorithm>
#include <cctype>

using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::FileSystem::Adapters;

// scenes are authored on Windows, so fall back to a case-insensitive lookup when the exact path is missing
static bool equalsIgnoreCase (const std::string& a, const std::string& b) {
    return std::ranges::equal (a, b, [] (unsigned char x, unsigned char y) { return std::tolower (x) == std::tolower (y); });
}

template <typename Files>
static auto findFile (const Files& files, const std::filesystem::path& path) {
    const auto wanted = path.string ();
    const auto exact = std::ranges::find_if (files, [&wanted] (const auto& file) { return file->filename == wanted; });

    if (exact != files.end ()) {
	return exact;
    }

    return std::ranges::find_if (files, [&wanted] (const auto& file) { return equalsIgnoreCase (file->filename, wanted); });
}

ReadStreamSharedPtr PackageAdapter::open (const std::filesystem::path& path) const {
    const auto it = findFile (this->package->files, path);

    if (it == this->package->files.end ()) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    auto buffer = std::make_unique<char[]> (it->get ()->length);

    this->package->file->base ().seekg (it->get ()->offset + this->package->baseOffset, std::ios::beg);
    this->package->file->next (buffer.get (), it->get ()->length);

    return std::make_shared<MemoryStream> (std::move (buffer), it->get ()->length);
}

bool PackageAdapter::exists (const std::filesystem::path& path) const {
    return findFile (this->package->files, path) != this->package->files.end ();
}

std::filesystem::path PackageAdapter::physicalPath (const std::filesystem::path& path) const {
    throw std::filesystem::filesystem_error ("Package adapter does not support realpath", path, std::error_code ());
}

std::optional<std::filesystem::path>
PackageAdapter::resolveWorkshopDependencyAlias (const std::filesystem::path& path) const {
    std::optional<std::filesystem::path> found = std::nullopt;

    for (const auto& file : this->package->files) {
	if (!isWorkshopDependencyAliasOf (file->filename, path)) {
	    continue;
	}

	if (found.has_value ()) {
	    // more than one dependency ships a same-named file under this path, refuse to guess
	    return std::nullopt;
	}

	found = file->filename;
    }

    return found;
}

bool PackageFactory::handlesMountpoint (const std::filesystem::path& path) const {
    try {
	const auto finalpath = std::filesystem::canonical (path);
	const auto status = std::filesystem::status (finalpath);

	return std::filesystem::exists (finalpath) && std::filesystem::is_regular_file (status)
	    && finalpath.extension () == ".pkg";
    } catch (std::filesystem::filesystem_error&) {
	return false;
    }
}

AdapterSharedPtr PackageFactory::create (const std::filesystem::path& path) const {
    const auto stream = std::make_shared<std::ifstream> (path, std::ios::binary);
    auto package = Data::Parsers::PackageParser::parse (stream);

    return std::make_unique<PackageAdapter> (std::move (package));
}
