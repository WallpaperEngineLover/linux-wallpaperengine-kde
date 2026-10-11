#include <filesystem>
#include <fstream>
#include <memory>

#include "MediaCover.h"

#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Media/MediaSource.h"

using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::FileSystem::Adapters;

ReadStreamSharedPtr MediaCoverAdapter::open (const std::filesystem::path& path) const {
    if (path != "$mediaThumbnail") {
	throw std::filesystem::filesystem_error (
	    "MediaCoverAdapter only supports $mediaThumbnail", path, std::error_code ()
	);
    }

    const auto& cover = source.getMediaInfo ().cover;

    if (!cover.has_value ()) {
	throw std::filesystem::filesystem_error ("Media source does not have a cover", path, std::error_code ());
    }

    const std::filesystem::path& file = *cover;

    if (std::filesystem::is_regular_file (file) == false) {
	throw std::filesystem::filesystem_error ("Media cover is not a regular file", file, std::error_code ());
    }

    return std::make_shared<std::ifstream> (file, std::ios::binary);
}

bool MediaCoverAdapter::exists (const std::filesystem::path& path) const { return path == ""; }

std::filesystem::path MediaCoverAdapter::physicalPath (const std::filesystem::path& path) const {
    sLog.exception ("MediaCoverAdapter does not support realpath");
}

bool MediaCoverFactory::handlesMountpoint (const std::filesystem::path& path) const {
    return path == "$mediaThumbnail";
}

AdapterSharedPtr MediaCoverFactory::create (const std::filesystem::path& path) const {
    if (path != "$mediaThumbnail") {
	sLog.exception ("MediaCoveradapter only supports $mediaThumbnail");
    }

    return std::make_unique<MediaCoverAdapter> (source);
}