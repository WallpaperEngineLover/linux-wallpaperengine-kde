#pragma once

#include <map>
#include <optional>
#include <string>

#include "WallpaperEngine/Data/JSON.h"

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;

// workshop properties store keys like "ui_background_mode_img", the strings live per language under
// general.localization
class Localization {
public:
    /** locale is something like "de-de", by default the one the user's environment asks for */
    explicit Localization (const std::optional<JSON>& general, std::string locale = systemLocale ());

    // readable text for a property label: user's language, then English, without WE's UI markup; unknown keys made
    // readable
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
