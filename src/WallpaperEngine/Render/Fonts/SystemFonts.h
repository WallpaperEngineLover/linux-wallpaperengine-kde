#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Fonts {
std::filesystem::path userFontDirectory ();

std::string fcMatch (const std::string& pattern, const std::string& format);

std::vector<std::string> systemFontFamilies (const std::string& font);

std::string installedFamilyFile (const std::vector<std::string>& families);

enum class FontStatus {
    Exact,
    Compatible,
    Substitute,
};

struct FontReport {
    std::string family;
    FontStatus status = FontStatus::Exact;
    std::string using_;
};

FontReport systemFontReport (const std::string& font);

FontReport emojiReport ();

bool containsEmoji (const std::string& utf8);
}
