#pragma once

#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"
#include "include/cef_browser.h"
#include "include/cef_render_handler.h"

#include <atomic>

namespace WallpaperEngine::WebBrowser::CEF {
// web host only: CEF's offscreen pixels go straight into shared memory; GetViewRect reports the size the main process
// asked for, runWebHost() calls WasResized()
class SharedMemoryRenderHandler : public CefRenderHandler {
public:
    // a socket (>= 0) is the dmabuf mode: the browser then hands over GPU buffers instead of pixels
    explicit SharedMemoryRenderHandler (
	WallpaperEngine::WebBrowser::IPC::WebHostSharedMemory* shm, int dmabufSocket = -1
    );

    ~SharedMemoryRenderHandler () override = default;

    void GetViewRect (CefRefPtr<CefBrowser> browser, CefRect& rect) override;

    void OnPaint (
	CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width,
	int height
    ) override;

    void OnAcceleratedPaint (
	CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects,
	const CefAcceleratedPaintInfo& info
    ) override;

    [[nodiscard]] uint32_t viewWidth () const { return this->m_viewWidth.load (std::memory_order_relaxed); }
    [[nodiscard]] uint32_t viewHeight () const { return this->m_viewHeight.load (std::memory_order_relaxed); }

    IMPLEMENT_REFCOUNTING (SharedMemoryRenderHandler);

private:
    WallpaperEngine::WebBrowser::IPC::WebHostSharedMemory* m_shm;
    int m_dmabufSocket;
    // slot currently owned by this side, painted into and then swapped with the shared one
    uint32_t m_backSlot = 0;
    std::atomic<uint32_t> m_viewWidth = 0;
    std::atomic<uint32_t> m_viewHeight = 0;
};
}
