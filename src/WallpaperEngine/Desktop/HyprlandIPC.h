#pragma once

#include <optional>
#include <string>

namespace WallpaperEngine::Desktop {
/**
 * The request socket of the Hyprland we run on, empty when there is none. HYPRLAND_INSTANCE_SIGNATURE names it; without
 * the variable (a Flatpak started outside the session's environment) or when it names a gone instance, the live ones in
 * $XDG_RUNTIME_DIR/hypr are looked at: the one whose hyprland.lock names our Wayland display, else the newest, but that
 * one only when XDG_CURRENT_DESKTOP (currentDesktop) names Hyprland, a nested or other session's instance isn't ours
 */
std::string findHyprlandSocket (
    const std::string& runtimeDir, const char* signature, const char* waylandDisplay, const char* currentDesktop
);

/** Sends one request ("j/cursorpos") and returns the whole reply, nullopt when the socket doesn't answer */
std::optional<std::string> hyprlandRequest (const std::string& socketPath, const std::string& request);
} // namespace WallpaperEngine::Desktop
