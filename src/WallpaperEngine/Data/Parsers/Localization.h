#pragma once

#include <map>
#include <optional>
#include <string>

#include "WallpaperEngine/Data/JSON.h"

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;

class Localization {
public:
    explicit Localization (const std::optional<JSON>& general, std::string locale = systemLocale ());

    [[nodiscard]] std::string resolve (const std::string& text) const;

    [[nodiscard]] static std::string clean (const std::string& text);

    [[nodiscard]] static std::string systemLocale ();

private:
    using Table = std::map<std::string, std::string>;

    std::map<std::string, Table> m_tables;
    const Table* m_preferred = nullptr;
    const Table* m_english = nullptr;
};
}
