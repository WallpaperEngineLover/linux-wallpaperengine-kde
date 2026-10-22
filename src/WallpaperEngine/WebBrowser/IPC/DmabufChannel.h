#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace WallpaperEngine::WebBrowser::IPC {
// one painted frame of the web host as dmabuf planes, passed over a unix socket with SCM_RIGHTS; the receiver owns the
// descriptors and closes them
struct DmabufFrame {
    static constexpr int MAX_PLANES = 4;

    uint32_t width = 0;
    uint32_t height = 0;
    // DRM fourcc of the buffer
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    int planeCount = 0;
    uint64_t sequence = 0;
    std::array<uint32_t, MAX_PLANES> stride {};
    std::array<uint64_t, MAX_PLANES> offset {};
    std::array<int, MAX_PLANES> fd { -1, -1, -1, -1 };

    void closeAll ();
};

// host side: sends a frame without blocking; false when the socket is full or gone (the frame is dropped, the
// descriptors stay the caller's)
bool sendDmabufFrame (int socket, const DmabufFrame& frame);

// main side: drains the socket and keeps the newest frame, older ones are closed. nullopt when nothing arrived;
// `closed` is set when the host's end is gone. Frames that don't fit maxWidth x maxHeight or lie about their planes
// are dropped
std::optional<DmabufFrame> receiveLatestDmabufFrame (int socket, uint32_t maxWidth, uint32_t maxHeight, bool& closed);

// two connected non blocking, close-on-exec sockets; false on failure
bool makeDmabufSocketPair (int& mainEnd, int& hostEnd);
}
