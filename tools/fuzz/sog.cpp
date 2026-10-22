#include "WallpaperEngine/Splat/SogLoader.h"

#include <cstring>
#include <string>

using namespace WallpaperEngine::Splat;

// the first two bytes give the length of the meta JSON, whatever follows is what every file of the cloud reads as
extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size) {
    if (size < 2) {
	return 0;
    }

    const size_t metaSize = std::min<size_t> ((data[0] | (data[1] << 8)), size - 2);
    const std::string meta (reinterpret_cast<const char*> (data) + 2, metaSize);
    const std::string payload (reinterpret_cast<const char*> (data) + 2 + metaSize, size - 2 - metaSize);

    try {
	std::ignore = loadSog (meta, [&] (const std::string&) { return payload; });
    } catch (const std::exception&) { }

    try {
	std::ignore = decodeWebP (payload);
    } catch (const std::exception&) { }

    return 0;
}
