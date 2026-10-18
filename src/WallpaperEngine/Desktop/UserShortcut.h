#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace WallpaperEngine::Desktop {
// what an assigned "usershortcut" points at: WE's JSON ({"commandtype": "file" | "directory" | "web" | "command",
// "file", "arguments", "application"}) or a plain string (.desktop file or id, URL, folder, file); commands only run
// when typed as one
struct UserShortcut {
    enum class Type { Application, File, Directory, Web, Command };

    Type type;
    std::string target;
    std::string arguments;

    [[nodiscard]] static std::optional<UserShortcut> parse (const std::string& value);

    [[nodiscard]] std::optional<std::filesystem::path> iconPath () const;

    bool launch () const;
};
}
