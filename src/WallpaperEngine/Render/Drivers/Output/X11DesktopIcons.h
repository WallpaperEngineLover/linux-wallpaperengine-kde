#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <X11/Xlib.h>

namespace WallpaperEngine::Render::Drivers::Output {
/**
 * Keeps the icons of an opaque desktop window (xfdesktop) visible above the wallpaper. The window stays where it is and
 * keeps handling every click, its background is turned into a key colour, the window's pixels are read through the
 * Composite extension whenever it is damaged and every pixel that is not the key colour is drawn over the wallpaper
 * frame before it goes to the screen.
 */
class X11DesktopIcons {
public:
    X11DesktopIcons (Display* display, Window root, int fullWidth, int fullHeight);
    ~X11DesktopIcons ();

    X11DesktopIcons (const X11DesktopIcons&) = delete;
    X11DesktopIcons& operator= (const X11DesktopIcons&) = delete;

    [[nodiscard]] static bool supports (Display* display, Window window);

    bool start (const std::vector<Window>& windows, const std::vector<std::string>& outputNames);

    void follow (const std::vector<Window>& windows);

    bool handleEvent (const XEvent& event);

    void update ();

    void composite (char* frame, int width, int height) const;

    [[nodiscard]] bool active () const { return this->m_active; }

private:
    struct Icon {
	uint32_t index;
	uint8_t b, g, r, a;
    };

    struct Tracked {
	Window window = None;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	unsigned long damage = 0;
	bool dirty = true;
	int attempts = 0;
	std::chrono::steady_clock::time_point retryAt = {};
	std::vector<Icon> icons;
    };

    void track (Window window);
    void untrack (Tracked& tracked);
    void capture (Tracked& tracked);

    Display* m_display;
    Window m_root;
    int m_fullWidth;
    int m_fullHeight;
    int m_compositeEvent = 0;
    int m_damageEvent = 0;
    bool m_active = false;
    std::vector<Tracked> m_tracked;
};
}
