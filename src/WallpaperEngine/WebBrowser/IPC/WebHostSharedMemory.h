#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace WallpaperEngine::WebBrowser::IPC {
// Shared-memory transport between the main engine process (reads frames, writes input/size) and a
// disposable "web host" child process that owns the actual CEF browser (writes frames, reads
// input/size). One segment per CWeb instance. The pixel buffers are allocated once at maxWidth x
// maxHeight (the wallpaper's maxRenderSize - see CWeb), but the browser can be resized within that
// cap for the lifetime of the host process by changing desiredWidth/desiredHeight; actual painted
// content may be smaller than the cap and is described by slotWidth/slotHeight.
struct WebHostSharedMemory {
    // Triple buffer for the pixel data. Three slots exist: the host owns one (the "back" slot, tracked
    // privately by the host, it paints into it), the main process owns one (the "front" slot, tracked
    // privately by the main process, it uploads from it) and the third sits here, in frameSlot. Publishing a
    // frame swaps the back slot with this one and marks it dirty, taking a frame swaps the front slot with
    // it. Both are a single atomic exchange, so neither side ever waits on the other or reads a half
    // written frame.
    // Bits 0-1 are the slot index, FRAME_DIRTY means the host published it and the main process hasn't
    // taken it yet. Starts as slot 1, clean (host begins on slot 0, main process on slot 2).
    static constexpr uint32_t FRAME_SLOTS = 3;
    static constexpr uint32_t FRAME_SLOT_MASK = 3;
    static constexpr uint32_t FRAME_DIRTY = 4;
    std::atomic<uint32_t> frameSlot { 1 };
    // Size of the content painted into each slot, written before the slot is published
    std::atomic<uint32_t> slotWidth[FRAME_SLOTS] {};
    std::atomic<uint32_t> slotHeight[FRAME_SLOTS] {};

    // Main process -> host process: bumped whenever the page should produce its next frame (CEF runs with
    // external begin frames). Doubles as a futex word, see waitForFrameRequest().
    std::atomic<uint32_t> frameRequestSeq { 0 };

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

    // Main process -> host process: latest audio spectrum (mono, 0..1) as produced by the recorder.
    // audioSeq is bumped after every update so the host only forwards it to the page when it changed;
    // a torn read across bands is harmless for a visualizer, so there's no seqlock here.
    static constexpr std::size_t AUDIO_BANDS = 64;
    std::atomic<uint32_t> audioSeq { 0 };
    std::atomic<float> audioBands[AUDIO_BANDS] {};

    // Main process -> host process: what the desktop's media player (MPRIS) is currently doing. The host process has
    // no D-Bus connection of its own, so the main process mirrors its MediaSource here. Guarded by a seqlock:
    // odd mediaSeq means a write is in progress, and a reader retries if it changed while reading.
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

    // Fixed capacity of the buffer below, set once at creation - never changes for the lifetime of
    // the segment.
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;

    // BGRA8 pixel data, FRAME_SLOTS slots of maxWidth * maxHeight * 4 bytes capacity each, immediately follow
    // this header in the same mapping. Only the top-left slotWidth x slotHeight of a slot holds live content.
    [[nodiscard]] uint8_t* frameBuffer (uint32_t slot) {
	return reinterpret_cast<uint8_t*> (this) + sizeof (WebHostSharedMemory)
	    + static_cast<std::size_t> (slot) * maxWidth * maxHeight * 4;
    }

    // Main process: asks the host to produce a frame and wakes it if it's sleeping in waitForFrameRequest()
    void requestFrame ();

    // Host process: sleeps until frameRequestSeq differs from lastSeen or timeoutMs passes. Returns the current
    // value, which the caller compares against lastSeen itself.
    uint32_t waitForFrameRequest (uint32_t lastSeen, uint32_t timeoutMs);

    [[nodiscard]] static std::size_t totalSize (uint32_t maxWidth, uint32_t maxHeight) {
	return sizeof (WebHostSharedMemory) + FRAME_SLOTS * static_cast<std::size_t> (maxWidth) * maxHeight * 4;
    }
};

// Creates a new, zero-initialized segment with capacity for maxWidth*maxHeight BGRA8 and maps it.
// Fills outName with a unique shm name the host process can be told to attach to. Returns nullptr
// on failure.
WebHostSharedMemory* createSharedMemory (std::string& outName, uint32_t maxWidth, uint32_t maxHeight);

// Removes what web host processes of engine processes that no longer exist left behind: shm segments named
// "lwe-web-<pid>-<n>" and CEF profile directories named "lwe-cef-<pid>-<uuid>" in the temp directory (the pid is
// the one that created it). Segments are 3 frames each, so a few killed engines add up to hundreds of MB of RAM.
void removeStaleWebHostFiles ();

// Attaches to a segment created by createSharedMemory() elsewhere (a different process). maxWidth
// and maxHeight must match what the creator used. Returns nullptr on failure.
WebHostSharedMemory* attachSharedMemory (const std::string& name, uint32_t maxWidth, uint32_t maxHeight);

// Unmaps a segment obtained from either function above. Only the creator should pass
// unlinkSegment=true (removes the shm name once both sides are done with it).
void closeSharedMemory (
    WebHostSharedMemory* mem, const std::string& name, uint32_t maxWidth, uint32_t maxHeight, bool unlinkSegment
);
} // namespace WallpaperEngine::WebBrowser::IPC
