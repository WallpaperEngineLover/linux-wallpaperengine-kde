#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace WallpaperEngine::WebBrowser::IPC {
// Shared-memory transport between the main engine process (reads frames, writes input/size) and a
// disposable "web host" child process that owns the actual CEF browser (writes frames, reads
// input/size). One segment per CWeb instance. The pixel buffer is allocated once at maxWidth x
// maxHeight (the wallpaper's maxRenderSize - see CWeb), but the browser can be resized within that
// cap for the lifetime of the host process by changing desiredWidth/desiredHeight; actual painted
// content may be smaller than the cap and is described by frameWidth/frameHeight.
struct WebHostSharedMemory {
    // Seqlock guarding the pixel buffer + frameWidth/frameHeight: the writer (host process) bumps
    // this to an odd value before writing and to the next even value after. A reader must retry if
    // it observes an odd value, or if the value changed between the start and end of its read.
    std::atomic<uint32_t> frameSeq { 0 };
    std::atomic<uint32_t> frameWidth { 0 };
    std::atomic<uint32_t> frameHeight { 0 };

    // Main process -> host process: desired browser viewport size. The host polls this and calls
    // CefBrowserHost::WasResized() when it changes.
    std::atomic<uint32_t> desiredWidth { 0 };
    std::atomic<uint32_t> desiredHeight { 0 };

    // Host process -> main process
    std::atomic<bool> helperReady { false };
    std::atomic<bool> helperFailed { false };

    // Main process -> host process
    std::atomic<bool> quitRequested { false };
    // Whether the browser's audio should be muted, see --audio-screen/--ambient-volume
    std::atomic<bool> audioMuted { false };
    std::atomic<double> mouseX { 0.0 };
    std::atomic<double> mouseY { 0.0 };
    // Matches WallpaperEngine::Input::MouseClickStatus (Released = 0, Clicked = 1)
    std::atomic<int32_t> leftClick { 0 };
    std::atomic<int32_t> rightClick { 0 };

    // Fixed capacity of the buffer below, set once at creation - never changes for the lifetime of
    // the segment.
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;

    // BGRA8 pixel data, maxWidth * maxHeight * 4 bytes capacity, immediately follows this header in
    // the same mapping. Only the top-left frameWidth x frameHeight of it holds live content.
    [[nodiscard]] uint8_t* frameBuffer () { return reinterpret_cast<uint8_t*> (this) + sizeof (WebHostSharedMemory); }

    [[nodiscard]] static std::size_t totalSize (uint32_t maxWidth, uint32_t maxHeight) {
	return sizeof (WebHostSharedMemory) + static_cast<std::size_t> (maxWidth) * maxHeight * 4;
    }
};

// Creates a new, zero-initialized segment with capacity for maxWidth*maxHeight BGRA8 and maps it.
// Fills outName with a unique shm name the host process can be told to attach to. Returns nullptr
// on failure.
WebHostSharedMemory* createSharedMemory (std::string& outName, uint32_t maxWidth, uint32_t maxHeight);

// Attaches to a segment created by createSharedMemory() elsewhere (a different process). maxWidth
// and maxHeight must match what the creator used. Returns nullptr on failure.
WebHostSharedMemory* attachSharedMemory (const std::string& name, uint32_t maxWidth, uint32_t maxHeight);

// Unmaps a segment obtained from either function above. Only the creator should pass
// unlinkSegment=true (removes the shm name once both sides are done with it).
void closeSharedMemory (
    WebHostSharedMemory* mem, const std::string& name, uint32_t maxWidth, uint32_t maxHeight, bool unlinkSegment
);
} // namespace WallpaperEngine::WebBrowser::IPC
