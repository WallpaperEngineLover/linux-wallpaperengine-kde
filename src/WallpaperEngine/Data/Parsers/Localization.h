#pragma once

#include <map>
#include <optional>
#include <string>

#include "WallpaperEngine/Data/JSON.h"

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;

/**
 * Resolves the texts of a project's properties. Workshop projects only store keys like "ui_background_mode_img" in
 * their properties and keep the actual strings, one table per language, under general.localization.
 */
class Localization {
public:
    /** locale is something like "de-de", by default the one the user's environment asks for */
    explicit Localization (const std::optional<JSON>& general, std::string locale = systemLocale ());

    /**
     * @return The readable text for a property text or option label: looked up in the user's language, then in
     * English, with the markup Wallpaper Engine's own UI renders (<small>, <hr>...) removed. Keys that are in no table
     * are made readable as far as that's possible.
     */
    [[nodiscard]] std::string resolve (const std::string& text) const;

    /** Drops markup and the parts meant as small print, and tidies whitespace */
    [[nodiscard]] static std::string clean (const std::string& text);

    /** The locale of the environment (LC_ALL, LC_MESSAGES, LANG) in the form the tables use, "en-us" if unset */
    [[nodiscard]] static std::string systemLocale ();

private:
    using Table = std::map<std::string, std::string>;

    std::map<std::string, Table> m_tables;
    const Table* m_preferred = nullptr;
    const Table* m_english = nullptr;
};
} // namespace WallpaperEngine::Data::Parsers
