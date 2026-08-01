#include "SharedMemoryRenderHandler.h"
#include <algorithm>
#include <cstring>

using namespace WallpaperEngine::WebBrowser::CEF;
using namespace WallpaperEngine::WebBrowser::IPC;

SharedMemoryRenderHandler::SharedMemoryRenderHandler (WebHostSharedMemory* shm) : m_shm (shm) { }

void SharedMemoryRenderHandler::GetViewRect (CefRefPtr<CefBrowser> browser, CefRect& rect) {
    const uint32_t width = std::clamp (this->m_shm->desiredWidth.load (std::memory_order_relaxed), 1u, this->m_shm->maxWidth);
    const uint32_t height
	= std::clamp (this->m_shm->desiredHeight.load (std::memory_order_relaxed), 1u, this->m_shm->maxHeight);

    rect = CefRect (0, 0, static_cast<int> (width), static_cast<int> (height));
}

void SharedMemoryRenderHandler::OnPaint (
    CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer,
    const int width, const int height
) {
    if (type != PET_VIEW) {
	return;
    }

    // GetViewRect always clamps to maxWidth/maxHeight, so CEF should never hand back anything
    // larger than the buffer's capacity - guard against it anyway rather than overflow the segment.
    if (width <= 0 || height <= 0 || static_cast<uint32_t> (width) > this->m_shm->maxWidth
	|| static_cast<uint32_t> (height) > this->m_shm->maxHeight) {
	return;
    }

    // Seqlock: only this thread (CEF's UI thread, one browser per host process) ever writes, so the
    // sequence is always even when we get here.
    const uint32_t seq = this->m_shm->frameSeq.load (std::memory_order_relaxed);
    this->m_shm->frameSeq.store (seq + 1, std::memory_order_release);
    this->m_shm->frameWidth.store (static_cast<uint32_t> (width), std::memory_order_relaxed);
    this->m_shm->frameHeight.store (static_cast<uint32_t> (height), std::memory_order_relaxed);
    std::memcpy (this->m_shm->frameBuffer (), buffer, static_cast<size_t> (width) * height * 4);
    this->m_shm->frameSeq.store (seq + 2, std::memory_order_release);
}
