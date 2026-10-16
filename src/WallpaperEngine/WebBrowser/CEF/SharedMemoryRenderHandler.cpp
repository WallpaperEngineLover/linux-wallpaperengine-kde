#include "SharedMemoryRenderHandler.h"
#include <algorithm>
#include <cstring>

using namespace WallpaperEngine::WebBrowser::CEF;
using namespace WallpaperEngine::WebBrowser::IPC;

SharedMemoryRenderHandler::SharedMemoryRenderHandler (WebHostSharedMemory* shm) : m_shm (shm) { }

void SharedMemoryRenderHandler::GetViewRect (CefRefPtr<CefBrowser> browser, CefRect& rect) {
    const uint32_t width
	= std::clamp (this->m_shm->desiredWidth.load (std::memory_order_relaxed), 1u, this->m_shm->maxWidth);
    const uint32_t height
	= std::clamp (this->m_shm->desiredHeight.load (std::memory_order_relaxed), 1u, this->m_shm->maxHeight);

    this->m_viewWidth.store (width, std::memory_order_relaxed);
    this->m_viewHeight.store (height, std::memory_order_relaxed);
    rect = CefRect (0, 0, static_cast<int> (width), static_cast<int> (height));
}

void SharedMemoryRenderHandler::OnPaint (
    CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer,
    const int width, const int height
) {
    if (type != PET_VIEW) {
	return;
    }

    // GetViewRect clamps to the capacity, guard anyway
    if (width <= 0 || height <= 0 || static_cast<uint32_t> (width) > this->m_shm->maxWidth
	|| static_cast<uint32_t> (height) > this->m_shm->maxHeight) {
	return;
    }

    // paint into the slot only this side owns, then hand it over with one exchange
    std::memcpy (this->m_shm->frameBuffer (this->m_backSlot), buffer, static_cast<size_t> (width) * height * 4);
    this->m_shm->slotWidth[this->m_backSlot].store (static_cast<uint32_t> (width), std::memory_order_relaxed);
    this->m_shm->slotHeight[this->m_backSlot].store (static_cast<uint32_t> (height), std::memory_order_relaxed);

    this->m_backSlot = this->m_shm->publishFrame (this->m_backSlot);
}
