#pragma once

#include <GL/glew.h>
#include <cstdint>
#include <list>
#include <memory>

#include "WallpaperEngine/WebBrowser/IPC/DmabufChannel.h"

namespace WallpaperEngine::Render::Wallpapers {
// main process side of --web-frames dmabuf: imports the web host's GPU buffers through EGL_EXT_image_dma_buf_import
// and copies them into the wallpaper's frame buffer with the GPU; the pixels never visit the CPU. The host's browser
// reuses a few buffers, so imported ones are kept by the buffer's identity and dropped when it stops coming
class WebDmabufImport {
public:
    // whether the current GL context sits on an EGL display that can import dmabufs (not GLX, not llvmpipe-less X11)
    [[nodiscard]] static bool available ();

    WebDmabufImport ();
    ~WebDmabufImport ();

    // copies frame.width x frame.height into the top left of the framebuffer; false when the buffer cannot be
    // imported or the GL driver refuses it. The caller closes the frame's descriptors, the import keeps its own
    [[nodiscard]] bool copyTo (const WallpaperEngine::WebBrowser::IPC::DmabufFrame& frame, GLuint framebuffer);

    // whether the frame holds more than one flat colour, judged on a grid of 48 x 27 samples (a page paints its
    // background long before its content)
    [[nodiscard]] bool framebufferHasContent (GLuint framebuffer, uint32_t width, uint32_t height);

    [[nodiscard]] uint64_t importedCount () const { return this->m_imports; }

private:
    struct Entry;

    Entry* find (const WallpaperEngine::WebBrowser::IPC::DmabufFrame& frame);
    Entry* import (const WallpaperEngine::WebBrowser::IPC::DmabufFrame& frame);

    std::list<std::unique_ptr<Entry>> m_entries;
    void* m_display;
    GLuint m_readFramebuffer = 0;
    GLuint m_gridFramebuffer = 0;
    GLuint m_gridTexture = 0;
    uint64_t m_imports = 0;
    bool m_errorLogged = false;
};
}
