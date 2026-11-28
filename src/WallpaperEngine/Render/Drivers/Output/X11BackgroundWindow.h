#pragma once

#include <cstdint>
#include <glm/vec4.hpp>
#include <vector>

namespace WallpaperEngine::Render::Drivers::Output {
// Xlib types stay out of this header, its macros clash with the rest of the engine
class X11BackgroundWindow {
public:
    /** visible: the areas of the window that show (x, y, width, height on the screen), the rest of the window is cut
     * away so another engine's window on another output is not covered; empty shows all of it */
    X11BackgroundWindow (int x, int y, int width, int height, const std::vector<glm::ivec4>& visible = {});
    ~X11BackgroundWindow ();

    X11BackgroundWindow (const X11BackgroundWindow&) = delete;
    X11BackgroundWindow& operator= (const X11BackgroundWindow&) = delete;

    [[nodiscard]] bool valid () const { return this->m_image != nullptr; }
    [[nodiscard]] char* data () const { return this->m_imageData; }
    [[nodiscard]] uint32_t size () const { return this->m_imageSize; }

    void present ();

private:
    void keepLow ();
    [[nodiscard]] bool desktopWindowBelowUs () const;
    [[nodiscard]] bool frameIsBlack () const;

    void* m_display = nullptr;
    unsigned long m_window = 0;
    void* m_gc = nullptr;
    void* m_image = nullptr;
    char* m_imageData = nullptr;
    uint32_t m_imageSize = 0;
    int m_width;
    int m_height;
    uint32_t m_updates = 0;
    unsigned long m_nudge = 0;
    uint32_t m_nudgeUntil = 0;
};
}
