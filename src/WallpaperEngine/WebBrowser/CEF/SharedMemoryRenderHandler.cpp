#include "SharedMemoryRenderHandler.h"
#include <algorithm>
#include <cstring>

#include "WallpaperEngine/WebBrowser/IPC/DmabufChannel.h"

using namespace WallpaperEngine::WebBrowser::CEF;
using namespace WallpaperEngine::WebBrowser::IPC;

SharedMemoryRenderHandler::SharedMemoryRenderHandler (WebHostSharedMemory* shm, const int dmabufSocket) :
    m_shm (shm), m_dmabufSocket (dmabufSocket) { }

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

void SharedMemoryRenderHandler::OnAcceleratedPaint (
    CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects,
    const CefAcceleratedPaintInfo& info
) {
    if (type != PET_VIEW || this->m_dmabufSocket < 0) {
	return;
    }

    // DRM_FORMAT_ARGB8888 / DRM_FORMAT_ABGR8888, little endian words: CEF's BGRA is the first, RGBA the second
    constexpr uint32_t DRM_ARGB8888 = 0x34325241;
    constexpr uint32_t DRM_ABGR8888 = 0x34324241;

    const uint32_t fourcc = info.format == CEF_COLOR_TYPE_BGRA_8888 ? DRM_ARGB8888
	: info.format == CEF_COLOR_TYPE_RGBA_8888                   ? DRM_ABGR8888
								    : 0;

    if (fourcc == 0 || info.plane_count < 1 || info.plane_count > IPC::DmabufFrame::MAX_PLANES) {
	this->m_shm->dmabufRejected.store (true, std::memory_order_release);
	return;
    }

    // three frames in flight at most: a main process that stopped drawing leaves them unread
    const uint64_t sent = this->m_shm->dmabufSent.load (std::memory_order_relaxed);

    if (sent - this->m_shm->dmabufTaken.load (std::memory_order_acquire) >= 3) {
	return;
    }

    IPC::DmabufFrame frame;
    const auto& visible = info.extra.visible_rect;
    frame.width = static_cast<uint32_t> (visible.width > 0 ? visible.width : info.extra.coded_size.width);
    frame.height = static_cast<uint32_t> (visible.height > 0 ? visible.height : info.extra.coded_size.height);
    frame.fourcc = fourcc;
    frame.modifier = info.modifier;
    frame.planeCount = info.plane_count;
    frame.sequence = sent + 1;

    for (int i = 0; i < info.plane_count; i++) {
	frame.stride[i] = info.planes[i].stride;
	frame.offset[i] = info.planes[i].offset;
	frame.fd[i] = info.planes[i].fd;
    }

    // the descriptors stay CEF's (the buffer goes back to its pool when this returns), the kernel duplicates them
    // into the main process
    if (IPC::sendDmabufFrame (this->m_dmabufSocket, frame)) {
	this->m_shm->dmabufSent.store (sent + 1, std::memory_order_release);
    }
}
