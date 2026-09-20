#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Parsers/PropertyParser.h"

using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Data::Parsers::PropertyParser;

TEST_CASE ("Text input properties keep their string value without JSON quotes") {
    const JSON propertyData = {
	{ "type", "textinput" },
	{ "text", "Format" },
	{ "value", "HH:mm" },
    };

    const auto property = PropertyParser::parse (propertyData, "format");

    REQUIRE (property != nullptr);
    CHECK (property->getType () == DynamicValue::String);
    CHECK (property->getString () == "HH:mm");
}
