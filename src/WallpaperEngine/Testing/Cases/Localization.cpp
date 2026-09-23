#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Parsers/Localization.h"
#include "WallpaperEngine/Data/Parsers/PropertyParser.h"

using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Parsers::Localization;

namespace {
JSON project () {
    return JSON {
	{ "localization",
	  { { "en-us",
	      { { "ui_background_mode", "Wallpaper Mode" },
		{ "ui_background_text", "<hr><h4>●  Setting background</h4><hr>" },
		{ "ui_color__highContrastColor", "High Contrast color <small>Black or white, whichever fits.</small>" },
		{ "ui_only_english", "Only in English" } } },
	    { "de-de", { { "ui_background_mode", "Hintergrundmodus" } } },
	    { "zh-chs", { { "ui_background_mode", "背景模式" } } } } },
    };
}
} // namespace

TEST_CASE ("Property texts are looked up in the localization tables") {
    const Localization localization (project (), "en-us");

    CHECK (localization.resolve ("ui_background_mode") == "Wallpaper Mode");
    CHECK (localization.resolve ("") == "");
}

TEST_CASE ("Markup and small print are dropped from localized texts") {
    const Localization localization (project (), "en-us");

    CHECK (localization.resolve ("ui_background_text") == "● Setting background");
    CHECK (localization.resolve ("ui_color__highContrastColor") == "High Contrast color");
}

TEST_CASE ("The user's language wins, English fills in what it is missing") {
    const Localization german (project (), "de-de");

    CHECK (german.resolve ("ui_background_mode") == "Hintergrundmodus");
    CHECK (german.resolve ("ui_only_english") == "Only in English");
}

TEST_CASE ("A language variant is picked when only the language matches") {
    const Localization chinese (project (), "zh-cn");

    CHECK (chinese.resolve ("ui_background_mode") == "背景模式");
}

TEST_CASE ("An unknown language falls back to English") {
    const Localization localization (project (), "fr-fr");

    CHECK (localization.resolve ("ui_background_mode") == "Wallpaper Mode");
}

TEST_CASE ("Keys without a translation are made readable, real text is left alone") {
    const Localization localization (std::nullopt, "en-us");

    CHECK (localization.resolve ("ui_music_type_0") == "Music type 0");
    CHECK (localization.resolve ("Scale") == "Scale");
    CHECK (localization.resolve ("Speed &amp; <b>power</b>") == "Speed & power");
}

TEST_CASE ("Property texts and combo labels of a whole project are localized") {
    const JSON data = {
	{ "title", "Test" },
	{ "type", "web" },
	{ "file", "index.html" },
	{ "general",
	  { { "localization", project ().at ("localization") },
	    { "properties",
	      { { "mode",
		  { { "type", "combo" },
		    { "text", "ui_background_mode" },
		    { "value", 1 },
		    { "options", JSON::array ({ { { "label", "ui_only_english" }, { "value", 1 } } }) } } } } } } },
    };

    const auto localization = Localization (data.optional ("general"), "en-us");
    (void) localization;

    // ProjectParser needs an asset locator to build a Project, so the pieces are checked separately above and the
    // combo relabeling here through the property itself
    const auto property = WallpaperEngine::Data::Parsers::PropertyParser::parse (
	data.at ("general").at ("properties").at ("mode"), "mode"
    );
    auto* combo = dynamic_cast<WallpaperEngine::Data::Model::PropertyCombo*> (property.get ());

    REQUIRE (combo != nullptr);

    combo->relabel ([&] (const std::string& label) { return localization.resolve (label); });

    CHECK (combo->dump ().find ("Only in English") != std::string::npos);
}
