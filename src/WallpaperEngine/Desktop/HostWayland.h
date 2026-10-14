#pragma once

struct wl_display;

namespace WallpaperEngine::Desktop {
/**
 * A Wayland connection for the engine. Inside a Flatpak the compositor sees the app through Flatpak's
 * wp_security_context socket, and Hyprland (and other wlroots compositors) hide zwlr_layer_shell_v1 and the foreign
 * toplevel list from sandboxed clients. When that socket has no layer shell, the connection comes from the helper
 * started on the host through flatpak-spawn, which connects to the compositor's own socket and hands it over. The app
 * can run host commands anyway (org.freedesktop.Flatpak), this gains nothing it didn't have. Null on failure.
 */
wl_display* connectWayland ();

/** Whether connectWayland goes through the host helper, decided on first use */
bool usesHostWayland ();
} // namespace WallpaperEngine::Desktop
