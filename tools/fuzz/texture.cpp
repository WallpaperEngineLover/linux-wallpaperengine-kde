#include "WallpaperEngine/Data/Parsers/TextureParser.h"
#include "WallpaperEngine/Data/Utils/BinaryReader.h"

#include <sstream>
#include <tuple>

using namespace WallpaperEngine::Data;

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* data, size_t size) {
    auto stream = std::make_shared<std::istringstream> (
	std::string (reinterpret_cast<const char*> (data), size), std::ios::binary
    );

    try {
	Utils::BinaryReader reader (stream);
	std::ignore = Parsers::TextureParser::parse (reader);
    } catch (const std::exception&) { }

    return 0;
}
