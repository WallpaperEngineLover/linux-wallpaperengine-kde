#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace WallpaperEngine::Desktop {
/**
 * Looks up icons the freedesktop way: the user's theme (KDE's kdeglobals), the themes it inherits, hicolor, then
 * /usr/share/pixmaps. Prefers scalable SVGs, then the biggest PNG. Each theme is indexed once on first use.
 */
class IconTheme {
public:
    static IconTheme& get ();

    /** An absolute path is returned as is when it exists, anything else is looked up as an icon name */
    [[nodiscard]] std::optional<std::filesystem::path> find (const std::string& icon);

private:
    IconTheme ();

    struct Candidate {
	std::filesystem::path path;
	int score;
    };

    const std::map<std::string, Candidate>& index (const std::string& theme);
    [[nodiscard]] std::vector<std::string> inherits (const std::string& theme) const;

    std::vector<std::filesystem::path> m_baseDirs;
    std::vector<std::string> m_themes;
    std::map<std::string, std::map<std::string, Candidate>> m_indexes;
};
} // namespace WallpaperEngine::Desktop
