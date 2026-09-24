#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace WallpaperEngine::Desktop {
/**
 * What a "usershortcut" property points at once the user assigned it, and how to open it. Values are either the
 * real engine's JSON ({"commandtype": "file" | "directory" | "web" | "command", "file": ..., "arguments": ...},
 * plus "application" for a .desktop file) or a plain string: a .desktop file or id, a URL, a folder or a file.
 * Commands are only ever run when explicitly typed as one.
 */
struct UserShortcut {
    enum class Type { Application, File, Directory, Web, Command };

    Type type;
    std::string target;
    std::string arguments;

    [[nodiscard]] static std::optional<UserShortcut> parse (const std::string& value);

    [[nodiscard]] std::optional<std::filesystem::path> iconPath () const;

    /** Starts it detached from the engine, false when it couldn't be started */
    bool launch () const;
};
} // namespace WallpaperEngine::Desktop
