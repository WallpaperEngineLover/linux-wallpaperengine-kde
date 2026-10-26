#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>

#include "WallpaperEngine/Render/Fonts/SystemFonts.h"

using namespace WallpaperEngine::Render::Fonts;

TEST_CASE ("emoji are told from text symbols", "[fonts]") {
    CHECK (containsEmoji ("Hello \xF0\x9F\x98\x80"));
    CHECK (containsEmoji ("\xE2\xAD\x90 star"));
    CHECK (containsEmoji ("1\xEF\xB8\x8F\xE2\x83\xA3"));
    CHECK_FALSE (containsEmoji ("Plain text, 12:30 and \xE2\x98\x85 \xE2\x99\xA5 \xE2\x86\x92"));
    CHECK_FALSE (containsEmoji ("\xE6\x96\x87\xE6\x9C\xAC\xE5\xB1\x82"));
    CHECK_FALSE (containsEmoji (""));
}

TEST_CASE ("windows font names map to their families", "[fonts]") {
    CHECK (systemFontFamilies ("systemfont_arial").front () == "Arial");
    CHECK (systemFontFamilies ("systemfont_segoe").front () == "Segoe UI");
    CHECK (systemFontFamilies ("fonts/own.ttf").empty ());
    CHECK (systemFontReport ("fonts/own.ttf").family.empty ());
    const auto generic = systemFontReport ("systemfont_sansserif");
    CHECK (generic.status == FontStatus::Exact);
    CHECK (generic.family == "sans-serif");
}

TEST_CASE ("the user font folder follows LWE_FONT_DIR", "[fonts]") {
    setenv ("LWE_FONT_DIR", "/tmp/lwe-test-fonts", 1);
    CHECK (userFontDirectory () == std::filesystem::path ("/tmp/lwe-test-fonts"));
    setenv ("LWE_FONT_DIR", "fonts", 1);
    CHECK (userFontDirectory () != std::filesystem::path ("fonts"));
    unsetenv ("LWE_FONT_DIR");
}
