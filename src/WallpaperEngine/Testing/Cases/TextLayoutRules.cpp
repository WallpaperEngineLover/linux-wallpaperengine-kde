#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

#include "WallpaperEngine/Render/Objects/TextLayout.h"

using WallpaperEngine::Render::Objects::TextAlign;
using WallpaperEngine::Render::Objects::TextLayout;
using WallpaperEngine::Render::Objects::TextLayoutParams;
using WallpaperEngine::Render::Objects::TextLayoutResult;

namespace {
std::string systemFont () {
    FILE* pipe = popen ("fc-match -f '%{file}' 'DejaVu Sans' 2>/dev/null", "r");

    if (pipe == nullptr) {
	return {};
    }

    std::string path;
    char buffer[512];

    while (fgets (buffer, sizeof (buffer), pipe) != nullptr) {
	path += buffer;
    }

    pclose (pipe);
    return std::filesystem::exists (path) ? path : std::string ();
}

float width (const TextLayoutResult& result) { return result.maxX - result.minX; }
} // namespace

TEST_CASE ("Text layout follows wallpaper64.exe's line rules") {
    const std::string font = systemFont ();

    if (font.empty ()) {
	SKIP ("no system font to lay text out with");
    }

    TextLayout layout;
    REQUIRE (layout.setPrimaryFont ({}, font));

    TextLayoutParams params { .size = 12.0f, .align = TextAlign::Left };

    SECTION ("one line keeps the font's ascender as the top") {
	const auto result = layout.layout ("Plain", params);

	REQUIRE (result.valid);
	CHECK (result.lines == 1);
	CHECK (result.top >= result.ascender);
	CHECK (result.bottom <= result.ascender - result.pitch);
	CHECK (result.quads.size () == 5);
    }

    SECTION ("spacing widens every advance and the line pitch") {
	const auto plain = layout.layout ("Aa\nAa", params);
	params.spacing = { 10.0f, 20.0f };
	const auto spaced = layout.layout ("Aa\nAa", params);

	REQUIRE (plain.valid);
	REQUIRE (spaced.valid);
	CHECK (spaced.pitch == plain.pitch + 20.0f);
	CHECK (width (spaced) == width (plain) + 10.0f);
    }

    SECTION ("limitwidth breaks at the last space and blockalign fills maxwidth") {
	params.maxWidth = 300.0f;
	const auto wrapped = layout.layout ("the quick brown fox jumps over the lazy dog", params);

	REQUIRE (wrapped.valid);
	CHECK (wrapped.lines > 1);
	CHECK (width (wrapped) <= 300.0f);

	params.blockAlign = true;
	const auto justified = layout.layout ("the quick brown fox jumps over the lazy dog", params);

	REQUIRE (justified.valid);
	CHECK (justified.lines == wrapped.lines);
	// wrapped lines span exactly maxwidth from their own left edge, the unwrapped last line may reach a pixel further
	CHECK (width (justified) >= 300.0f);
	CHECK (width (justified) <= 302.0f);
    }

    SECTION ("limitrows cuts the text and fits an ellipsis into maxwidth") {
	params.maxWidth = 300.0f;
	params.maxRows = 1;
	params.ellipsis = true;
	const auto result = layout.layout ("the quick brown fox jumps over the lazy dog", params);

	REQUIRE (result.valid);
	CHECK (result.lines == 1);
	CHECK (result.maxX <= 300.0f);
    }

    SECTION ("a row limit also drops whole lines") {
	params.maxRows = 2;
	const auto result = layout.layout ("one\ntwo\nthree", params);

	REQUIRE (result.valid);
	CHECK (result.lines == 2);
    }

    SECTION ("MSDF glyph quads carry 12 atlas pixels of range around the glyph") {
	const auto plain = layout.layout ("A", params);
	params.msdf = true;
	const auto msdf = layout.layout ("A", params);

	REQUIRE (plain.valid);
	REQUIRE (msdf.valid);
	REQUIRE (msdf.quads.size () == 1);
	CHECK (layout.getAtlasChannels () == 4);
	CHECK (msdf.quads[0].rect.x < plain.quads[0].rect.x);
	CHECK (msdf.quads[0].rect.w > plain.quads[0].rect.w);
	// the box is the glyph's, the range outside it doesn't count
	CHECK (msdf.maxX == plain.maxX);
    }
}
