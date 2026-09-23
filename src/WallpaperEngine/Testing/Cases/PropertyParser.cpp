#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Parsers/PropertyParser.h"

using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Data::Parsers::PropertyParser;

TEST_CASE ("Bool properties without a value default to false") {
    const JSON propertyData = {
	{ "type", "bool" },
	{ "text", "Enabled" },
    };

    const auto property = PropertyParser::parse (propertyData, "enabled");

    REQUIRE (property != nullptr);
    CHECK (property->getType () == DynamicValue::Boolean);
    CHECK_FALSE (property->getBool ());
}

TEST_CASE ("Directory properties are parsed as file-like properties") {
    const JSON propertyData = {
	{ "type", "directory" },
	{ "text", "Folder" },
    };

    const auto property = PropertyParser::parse (propertyData, "folder");

    REQUIRE (property != nullptr);
    CHECK (property->dump ().find ("folder - file") != std::string::npos);
}
TEST_CASE ("Directory properties remember they are directories and what file type they hold") {
    const auto directory = PropertyParser::parse (
	JSON { { "type", "directory" }, { "text", "Videos" }, { "fileType", "video" }, { "mode", "fetchall" } }, "vidDir"
    );
    const auto file = PropertyParser::parse (JSON { { "type", "file" }, { "text", "Image" } }, "image");

    const auto* asDirectory = dynamic_cast<const WallpaperEngine::Data::Model::PropertyFile*> (directory.get ());
    const auto* asFile = dynamic_cast<const WallpaperEngine::Data::Model::PropertyFile*> (file.get ());

    REQUIRE (asDirectory != nullptr);
    REQUIRE (asFile != nullptr);
    CHECK (asDirectory->isDirectory ());
    CHECK (asDirectory->getFileType () == "video");
    CHECK_FALSE (asFile->isDirectory ());
    CHECK (asFile->getFileType ().empty ());
}

TEST_CASE ("UI-only description lines with made up types are parsed as plain text") {
    for (const char* type : { "txt", "test", "label" }) {
	const JSON propertyData = {
	    { "type", type },
	    { "text", "ui_some_tip" },
	    { "value", "1 0 0" },
	};

	CHECK (PropertyParser::parse (propertyData, "tip") != nullptr);
    }
}

TEST_CASE ("Combo properties can be given a value the wallpaper did not list") {
    const JSON propertyData = {
	{ "type", "combo" },
	{ "text", "Interval" },
	{ "value", 1 },
	{ "options", JSON::array ({ { { "label", "1 min" }, { "value", 1 } }, { { "label", "5 min" }, { "value", 5 } } }) },
    };

    const auto property = PropertyParser::parse (propertyData, "interval");
    auto* combo = dynamic_cast<WallpaperEngine::Data::Model::PropertyCombo*> (property.get ());

    REQUIRE (combo != nullptr);

    combo->update (std::string ("0.5"), DynamicValue::UpdateSource::User);
    CHECK (combo->getString () == "1");

    combo->allowValue ("0.5");
    combo->update (std::string ("0.5"), DynamicValue::UpdateSource::User);
    CHECK (combo->getString () == "0.5");
}
