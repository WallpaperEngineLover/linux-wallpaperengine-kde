#include "WallpaperEngine/Data/Parsers/PackageParser.h"
#include "WallpaperEngine/FileSystem/Adapters/Package.h"

#include <iterator>
#include <sstream>

using namespace WallpaperEngine;

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size) {
    auto stream = std::make_shared<std::istringstream> (
	std::string (reinterpret_cast<const char*> (data), size), std::ios::binary
    );

    try {
	FileSystem::Adapters::PackageAdapter adapter (Data::Parsers::PackageParser::parse (stream));

	for (const auto& entry : adapter.package->files) {
	    try {
		const auto handle = adapter.open (entry->filename);
		std::string content ((std::istreambuf_iterator<char> (*handle)), std::istreambuf_iterator<char> ());
	    } catch (const std::exception&) { }
	}
    } catch (const std::exception&) { }

    return 0;
}
