#pragma once

#include "WallpaperEngine/WebBrowser/IPC/WebHostSharedMemory.h"
#include "include/cef_browser.h"
#include "include/cef_render_handler.h"

#include <atomic>

namespace WallpaperEngine::WebBrowser::CEF {
// Used only inside the disposable web-host child process (see WallpaperApplication::runWebHost()).
// Has no GL context and no CWeb to talk to directly - it writes CEF's off-screen-rendered pixels
// straight into the shared memory segment the main process reads frames from. GetViewRect reports
// whatever size the main process last asked for via shm - the caller (runWebHost()) is responsible
// for calling CefBrowserHost::WasResized() when that changes.
class SharedMemoryRenderHandler : public CefRenderHandler {
public:
    explicit SharedMemoryRenderHandler (WallpaperEngine::WebBrowser::IPC::WebHostSharedMemory* shm);

    ~SharedMemoryRenderHandler () override = default;

    void GetViewRect (CefRefPtr<CefBrowser> browser, CefRect& rect) override;

    void OnPaint (
	CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width,
	int height
    ) override;

    /** 0x0 before CEF asked */
    [[nodiscard]] uint32_t viewWidth () const { return this->m_viewWidth.load (std::memory_order_relaxed); }
    [[nodiscard]] uint32_t viewHeight () const { return this->m_viewHeight.load (std::memory_order_relaxed); }

    IMPLEMENT_REFCOUNTING (SharedMemoryRenderHandler);

private:
    WallpaperEngine::WebBrowser::IPC::WebHostSharedMemory* m_shm;
    // slot currently owned by this side, painted into and then swapped with the shared one
    uint32_t m_backSlot = 0;
    std::atomic<uint32_t> m_viewWidth = 0;
    std::atomic<uint32_t> m_viewHeight = 0;
};
} // namespace WallpaperEngine::WebBrowser::CEF
