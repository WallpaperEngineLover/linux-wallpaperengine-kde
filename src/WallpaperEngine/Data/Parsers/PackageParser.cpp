#include "PackageParser.h"

#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Data/Assets/Package.h"

#include <fstream>
#include <memory>
#include <tuple>

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Assets;
using namespace WallpaperEngine::Data::Utils;

PackageUniquePtr PackageParser::parse (ReadStreamSharedPtr stream) {
    auto reader = std::make_unique<BinaryReader> (std::move (stream));

    if (const std::string header = reader->nextSizedString (); header.starts_with ("PKGV") == false) {
	sLog.exception ("Expected header to start with PKGV, got ", header);
    }

    auto result = std::make_unique<Package> (Package {
	.file = std::move (reader),
    });

    result->files = parseFileList (*result->file);
    result->baseOffset = result->file->base ().tellg ();
    // looked up once here, the adapter's bounds checks reuse it
    std::ignore = result->file->size ();

    return result;
}

FileEntryList PackageParser::parseFileList (const BinaryReader& stream) {
    FileEntryList result = {};
    const uint32_t filesCount = stream.nextUInt32 ();

    // each entry is a name length, a name, an offset and a length: the count can't be more than the file holds
    if (!stream.fits (filesCount, 3 * sizeof (uint32_t))) {
	sLog.exception ("Package lists ", filesCount, " files, more than it can hold");
    }

    result.reserve (filesCount);

    for (uint32_t i = 0; i < filesCount; i++) {
	result.push_back (
	    std::make_unique<FileEntry> (FileEntry {
		.filename = stream.nextSizedString (), .offset = stream.nextUInt32 (), .length = stream.nextUInt32 () })
	);
    }

    return result;
}
