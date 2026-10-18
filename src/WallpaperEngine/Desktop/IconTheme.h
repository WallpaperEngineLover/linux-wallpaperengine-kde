#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace WallpaperEngine::Desktop {
class IconTheme {
public:
    static IconTheme& get ();

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
}
