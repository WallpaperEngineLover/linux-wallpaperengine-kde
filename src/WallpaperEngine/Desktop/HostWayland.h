#pragma once

struct wl_display;

namespace WallpaperEngine::Desktop {
// a Wayland connection for the engine: Flatpak's wp_security_context socket hides layer shell and foreign toplevels on
// wlroots compositors (Hyprland), then the connection comes from a host helper via flatpak-spawn (nothing the app
// couldn't already do). Null on failure
wl_display* connectWayland ();

bool usesHostWayland ();

// a native Wayland session whose compositor can't put a surface behind the windows (no wlr-layer-shell, not GNOME): the
// engine then draws through XWayland like on an X11 desktop
bool compositorLacksLayerShell ();
}
