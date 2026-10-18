#pragma once

#include <optional>
#include <string>

namespace WallpaperEngine::Desktop {
// the request socket of our Hyprland, empty when there is none: HYPRLAND_INSTANCE_SIGNATURE, else (Flatpak, stale
// variable) the live instance in $XDG_RUNTIME_DIR/hypr whose hyprland.lock names our display, else the newest one when
// XDG_CURRENT_DESKTOP (currentDesktop) names Hyprland
std::string findHyprlandSocket (
    const std::string& runtimeDir, const char* signature, const char* waylandDisplay, const char* currentDesktop
);

std::optional<std::string> hyprlandRequest (const std::string& socketPath, const std::string& request);
}
