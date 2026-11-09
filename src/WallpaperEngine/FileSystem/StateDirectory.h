#pragma once

#include <cstdlib>
#include <filesystem>

namespace WallpaperEngine::FileSystem {
/** $XDG_STATE_HOME/linux-wallpaperengine, else ~/.local/state/linux-wallpaperengine, empty when neither is known */
inline std::filesystem::path stateRoot () {
    if (const char* state = std::getenv ("XDG_STATE_HOME"); state != nullptr && state[0] == '/') {
	return std::filesystem::path (state) / "linux-wallpaperengine";
    }

    if (const char* home = std::getenv ("HOME"); home != nullptr && home[0] != '\0') {
	return std::filesystem::path (home) / ".local" / "state" / "linux-wallpaperengine";
    }

    return {};
}
}
