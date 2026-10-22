#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace WallpaperEngine::WebBrowser::IPC {
// shared memory between the main process (reads frames, writes input/size) and the web host (owns CEF, writes frames),
// one segment per CWeb; pixel buffers have maxWidth x maxHeight capacity, the browser resizes within it via
// desiredWidth/Height, the painted size is in slotWidth/slotHeight
struct WebHostSharedMemory {
    // triple buffer: the host paints its back slot, the main process uploads its front slot, the third sits in
    // frameSlot; publishing and taking are single atomic swaps bits 0-1 are the slot, FRAME_DIRTY marks a published
    // frame not taken yet. Starts as slot 1 clean (host on 0, main on 2)
    static constexpr uint32_t FRAME_SLOTS = 3;
    static constexpr uint32_t FRAME_SLOT_MASK = 3;
    static constexpr uint32_t FRAME_DIRTY = 4;
    std::atomic<uint32_t> frameSlot { 1 };
    // Size of the content painted into each slot, written before the slot is published
    std::atomic<uint32_t> slotWidth[FRAME_SLOTS] {};
    std::atomic<uint32_t> slotHeight[FRAME_SLOTS] {};

    // dmabuf frames (--web-frames dmabuf) travel over a socket instead of the slots above. Host: frames sent so far,
    // main: the newest one it took; the host stops sending three frames ahead, so a main process that isn't drawing
    // (paused) doesn't make it pile up buffers. dmabufRejected is the host saying the buffers it got can't be shared
    std::atomic<uint64_t> dmabufSent { 0 };
    std::atomic<uint64_t> dmabufTaken { 0 };
    std::atomic<bool> dmabufRejected { false };

    // Main process -> host process: bumped whenever the page should produce its next frame (CEF runs with
    // external begin frames). Doubles as a futex word, see waitForFrameRequest().
    std::atomic<uint32_t> frameRequestSeq { 0 };

    // desired viewport size, the host calls WasResized() when it changes
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

    // Main -> host: latest [left 64 | right 64] bands, unprocessed. Torn reads are harmless here, no seqlock
    static constexpr std::size_t AUDIO_BANDS = 128;
    std::atomic<float> audioBands[AUDIO_BANDS] {};

    // the desktop media player (MPRIS) mirrored for the host, which has no D-Bus; seqlock, odd mediaSeq while writing
    static constexpr std::size_t MEDIA_TEXT = 256;
    static constexpr std::size_t MEDIA_PATH = 1024;
    std::atomic<uint32_t> mediaSeq { 0 };
    // Bumped only when the cover changed, so the host doesn't reload and re-encode it for every progress update
    uint32_t mediaCoverVersion = 0;
    // Matches WallpaperEngine::Media::MediaSource::PlaybackState
    int32_t mediaState = 0;
    bool mediaAvailable = false;
    double mediaPosition = 0.0;
    double mediaDuration = 0.0;
    char mediaTitle[MEDIA_TEXT] = {};
    char mediaArtist[MEDIA_TEXT] = {};
    char mediaAlbum[MEDIA_TEXT] = {};
    // Local file of the cover, empty if there is none (or it isn't a file:// URL)
    char mediaCoverPath[MEDIA_PATH] = {};
    // primary, secondary, tertiary, text and high contrast as RGB8, see Media::ThumbnailPalette
    uint8_t mediaPalette[5][3] = {};

    // name=value lines, odd sequence numbers while writing
    static constexpr std::size_t PROPERTIES_TEXT = 65536;
    std::atomic<uint32_t> propertiesSeq { 0 };
    char propertiesText[PROPERTIES_TEXT] = {};

    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;

    // FRAME_SLOTS BGRA8 slots of maxWidth * maxHeight follow this header, only slotWidth x slotHeight is live
    [[nodiscard]] uint8_t* frameBuffer (uint32_t slot) { return frameBuffer (slot, maxWidth, maxHeight); }

    // for the side that must not take the capacity from the header, which the other process can write
    [[nodiscard]] uint8_t* frameBuffer (uint32_t slot, uint32_t capacityWidth, uint32_t capacityHeight) {
	return reinterpret_cast<uint8_t*> (this) + sizeof (WebHostSharedMemory)
	    + static_cast<std::size_t> (slot) * capacityWidth * capacityHeight * 4;
    }

    // whether a slot number and painted size another process wrote fit the segment as its creator sized it
    [[nodiscard]] static bool
    frameFits (uint32_t slot, uint32_t width, uint32_t height, uint32_t capacityWidth, uint32_t capacityHeight) {
	return slot < FRAME_SLOTS && width <= capacityWidth && height <= capacityHeight;
    }

    // host: publish the painted slot, get the next one
    uint32_t publishFrame (uint32_t backSlot) {
	return frameSlot.exchange (backSlot | FRAME_DIRTY, std::memory_order_acq_rel) & FRAME_SLOT_MASK;
    }

    // main process: swap the front slot for the latest frame, nullopt when none
    std::optional<uint32_t> takeFrame (uint32_t frontSlot) {
	if ((frameSlot.load (std::memory_order_acquire) & FRAME_DIRTY) == 0) {
	    return std::nullopt;
	}

	return frameSlot.exchange (frontSlot, std::memory_order_acq_rel) & FRAME_SLOT_MASK;
    }

    // Main process: asks the host to produce a frame and wakes it if it's sleeping in waitForFrameRequest()
    void requestFrame ();

    // host: sleeps until frameRequestSeq differs from lastSeen or the timeout, returns the current value
    uint32_t waitForFrameRequest (uint32_t lastSeen, uint32_t timeoutMs);

    [[nodiscard]] static std::size_t totalSize (uint32_t maxWidth, uint32_t maxHeight) {
	return sizeof (WebHostSharedMemory) + FRAME_SLOTS * static_cast<std::size_t> (maxWidth) * maxHeight * 4;
    }
};

// a zeroed segment for maxWidth*maxHeight BGRA8, outName is what the host attaches to; nullptr on failure
WebHostSharedMemory* createSharedMemory (std::string& outName, uint32_t maxWidth, uint32_t maxHeight);

// the frame slots stay untouched in dmabuf mode: gives their pages back (the segment keeps its header), and takes
// them again when the engine goes back to shared memory frames
void setFrameSlotsResident (const std::string& name, uint32_t maxWidth, uint32_t maxHeight, bool resident);

// removes what dead engines' web hosts left: "lwe-web-<tag>-<n>" segments and "lwe-cef-<tag>-<uuid>" profiles
// (Desktop::processTag); users hold a shared flock (CEF_PROFILE_LOCK for profiles), unlocked ones are stale unless the
// tag is a running pid. Each segment is 3 frames, hundreds of MB add up
void removeStaleWebHostFiles ();

// The file in a CEF profile directory its host process keeps locked
inline constexpr const char* CEF_PROFILE_LOCK = "lwe-host.lock";

// Whether some process holds a flock on the file
bool isLockedByAnyone (const std::string& path);

// attaches to a segment from createSharedMemory() in another process, same max size; nullptr on failure
WebHostSharedMemory* attachSharedMemory (const std::string& name, uint32_t maxWidth, uint32_t maxHeight);

// only the creator unlinks the shm name
void closeSharedMemory (
    WebHostSharedMemory* mem, const std::string& name, uint32_t maxWidth, uint32_t maxHeight, bool unlinkSegment
);
}
