#pragma once

#include <cstdint>

namespace WallpaperEngine::Render::Drivers::Output {
// Xlib types stay out of this header, its macros clash with the rest of the engine
class X11BackgroundWindow {
public:
    X11BackgroundWindow (int x, int y, int width, int height);
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
