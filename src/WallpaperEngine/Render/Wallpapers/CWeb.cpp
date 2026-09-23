// This code is a modification of the original projects that can be found at
// https://github.com/if1live/cef-gl-example
// https://github.com/andmcgregor/cefgui
#include "CWeb.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Media/ThumbnailPalette.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;
using namespace WallpaperEngine::WebBrowser::IPC;

namespace {
// Closes every fd but stdio, so the exec'd CEF child doesn't inherit the parent's
// Wayland/EGL/X11/audio connections. Must be async-signal-safe: this runs in the fork()ed child
// of a process that may have other threads running (SDL audio), so only a raw syscall - never
// opendir/readdir, which allocate - is safe to call here before execv().
void closeInheritedFds () {
#if defined(SYS_close_range)
    syscall (SYS_close_range, 3u, ~0u, 0u);
#endif
}
} // namespace

CWeb::CWeb (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const std::filesystem::path& resolvedBackgroundPath, const WallpaperState::TextureUVsScaling& scalingMode,
    const uint32_t& clampMode, const glm::ivec2& maxRenderSize
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode) {
    this->setupFramebuffers ();

    // capacity for the shared frame buffer, clamped to by setSize() below - deliberately
    // maxRenderSize and not Output::getFullWidth/Height() (see fromWallpaper's doc comment), which
    // would badly oversize this for the common case of several separate, non-spanned wallpapers.
    this->m_shmMaxWidth = static_cast<uint32_t> (std::max (1, maxRenderSize.x));
    this->m_shmMaxHeight = static_cast<uint32_t> (std::max (1, maxRenderSize.y));

    this->m_shm = createSharedMemory (this->m_shmName, this->m_shmMaxWidth, this->m_shmMaxHeight);

    if (this->m_shm == nullptr) {
	sLog.exception ("CWeb: failed to create shared memory for the web host process");
    }

    this->m_shm->desiredWidth.store (static_cast<uint32_t> (this->m_width), std::memory_order_relaxed);
    this->m_shm->desiredHeight.store (static_cast<uint32_t> (this->m_height), std::memory_order_relaxed);

    // pushed from the recorder's capture thread, the host process only ever reads these atomics
    auto* shm = this->m_shm;

    this->m_spectrumListenerId = this->getAudioContext ().getRecorder ().addSpectrumListener (
	[shm] (const float* audio64) {
	    for (std::size_t i = 0; i < WebHostSharedMemory::AUDIO_BANDS; i++) {
		shm->audioBands[i].store (audio64[i], std::memory_order_relaxed);
	    }

	    shm->audioSeq.fetch_add (1, std::memory_order_release);
	}
    );

    this->m_statsEnabled = std::getenv ("LWE_WEB_STATS") != nullptr;

    this->spawnHost (resolvedBackgroundPath);
}

void CWeb::spawnHost (const std::filesystem::path& resolvedBackgroundPath) {
    auto& appContext = this->getContext ().getApp ().getContext ();

    const pid_t pid = fork ();

    if (pid < 0) {
	// sLog.exception() throws, aborting construction before ~CWeb() ever runs - without this,
	// the named shm segment createSharedMemory() already made would never be unlinked.
	closeSharedMemory (this->m_shm, this->m_shmName, this->m_shmMaxWidth, this->m_shmMaxHeight, true);
	sLog.exception ("CWeb: fork() failed while spawning the web host process");
    }

    if (pid == 0) {
	closeInheritedFds ();

	// kill this child if the parent dies without telling it to quit over shared memory
	prctl (PR_SET_PDEATHSIG, SIGTERM);

	std::vector<std::string> args;
	args.emplace_back (resolvedBackgroundPath.string ());
	args.emplace_back ("--web-host");
	args.emplace_back ("--web-host-shm");
	args.emplace_back (this->m_shmName);
	args.emplace_back ("--web-host-width");
	args.emplace_back (std::to_string (this->m_shmMaxWidth));
	args.emplace_back ("--web-host-height");
	args.emplace_back (std::to_string (this->m_shmMaxHeight));

	if (!appContext.settings.general.assets.empty ()) {
	    args.emplace_back ("--assets-dir");
	    args.emplace_back (appContext.settings.general.assets.string ());
	}

	// the host reads the wallpaper's project on its own, so it only knows about property values that were passed on
	// the command line or through a hotswap request if they are handed to it again
	for (const auto& [name, value] : appContext.settings.general.properties) {
	    args.emplace_back ("--set-property");
	    args.emplace_back (name + "=" + value);
	}

	std::vector<char*> argv;
	argv.reserve (args.size () + 2);
	// cosmetic only (what the child sees as its own argv[0]) - the executable actually run is
	// /proc/self/exe below, not this string, so it doesn't matter whether the original argv[0]
	// was absolute, relative, or PATH-resolved by the shell that launched us
	argv.push_back (appContext.getArgv ()[0]);

	for (auto& arg : args) {
	    argv.push_back (arg.data ());
	}

	argv.push_back (nullptr);

	execv ("/proc/self/exe", argv.data ());

	// only reached if execv() itself failed
	_exit (127);
    }

    this->m_hostPid = pid;
}

void CWeb::setAudioPolicy (bool muted, std::optional<int> ambientVolume) {
    if (this->m_shm == nullptr) {
	return;
    }

    const bool shouldMute = muted || (ambientVolume.has_value () && *ambientVolume == 0);

    this->m_shm->audioMuted.store (shouldMute, std::memory_order_relaxed);
}

void CWeb::setSize (const int width, const int height) {
    this->m_width = width > 0 ? width : this->m_width;
    this->m_height = height > 0 ? height : this->m_height;

    if (this->m_width <= 0 || this->m_height <= 0) {
	return;
    }

    // clamp to the capacity decided at construction time (m_shmMaxWidth/Height) - the host process
    // notices this changed and calls WasResized() itself, see runWebHost()
    this->m_shm->desiredWidth.store (
	std::min (static_cast<uint32_t> (this->m_width), this->m_shmMaxWidth), std::memory_order_relaxed
    );
    this->m_shm->desiredHeight.store (
	std::min (static_cast<uint32_t> (this->m_height), this->m_shmMaxHeight), std::memory_order_relaxed
    );
}

void CWeb::renderFrame (const glm::ivec4& viewport) {
    if (!this->m_helperFailureLogged && this->m_shm->helperFailed.load (std::memory_order_relaxed)) {
	sLog.error ("CWeb: host process failed to start, this web wallpaper will not render");
	this->m_helperFailureLogged = true;
    }

    if (viewport.z != this->getWidth () || viewport.w != this->getHeight ()) {
	this->setSize (viewport.z, viewport.w);
    }

    this->updateMouse (viewport);
    this->updateMedia ();
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    glViewport (0, 0, this->getWidth (), this->getHeight ());

    if ((this->m_shm->frameSlot.load (std::memory_order_acquire) & WebHostSharedMemory::FRAME_DIRTY) == 0) {
	// nothing new, but the next frame is still due
	this->requestPageFrame ();
	return;
    }

    const uint32_t taken = this->m_shm->frameSlot.exchange (this->m_frontSlot, std::memory_order_acq_rel);
    this->m_frontSlot = taken & WebHostSharedMemory::FRAME_SLOT_MASK;

    const uint32_t frameWidth = this->m_shm->slotWidth[this->m_frontSlot].load (std::memory_order_relaxed);
    const uint32_t frameHeight = this->m_shm->slotHeight[this->m_frontSlot].load (std::memory_order_relaxed);

    if (frameWidth == 0 || frameHeight == 0) {
	this->requestPageFrame ();
	return;
    }

    glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());

    // only reallocate when the size changed, glTexSubImage2D avoids a driver-side realloc/sync every frame
    if (frameWidth == this->m_uploadedTextureWidth && frameHeight == this->m_uploadedTextureHeight) {
	glTexSubImage2D (
	    GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei> (frameWidth), static_cast<GLsizei> (frameHeight),
	    GL_BGRA_EXT, GL_UNSIGNED_BYTE, this->m_shm->frameBuffer (this->m_frontSlot)
	);
    } else {
	glTexImage2D (
	    GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei> (frameWidth), static_cast<GLsizei> (frameHeight), 0,
	    GL_BGRA_EXT, GL_UNSIGNED_BYTE, this->m_shm->frameBuffer (this->m_frontSlot)
	);
	this->m_uploadedTextureWidth = frameWidth;
	this->m_uploadedTextureHeight = frameHeight;
    }

    glBindTexture (GL_TEXTURE_2D, 0);

    this->m_statsFrames++;

    // ask for the next frame only after taking this one, so what gets painted is always what the next render shows
    this->requestPageFrame ();
}

void CWeb::requestPageFrame () {
    const auto now = std::chrono::steady_clock::now ();

    if (this->m_lastRender != std::chrono::steady_clock::time_point {}) {
	const double interval = std::chrono::duration<double> (now - this->m_lastRender).count ();

	// a long gap is a pause or a stall, not the rate we're rendering at
	if (interval < 0.1) {
	    this->m_renderInterval = this->m_renderInterval == 0.0 ? interval : this->m_renderInterval * 0.95 + interval * 0.05;

	    // renders per 60Hz frame, only switching once clearly closer to another whole number so a rate sitting
	    // between two (90Hz) doesn't flip back and forth
	    const double ratio = 1.0 / (this->m_renderInterval * 60.0);

	    if (std::abs (ratio - this->m_frameRequestDivisor) > 0.65) {
		this->m_frameRequestDivisor = static_cast<uint32_t> (std::max (1L, std::lround (ratio)));
	    }
	}
    }

    this->m_lastRender = now;

    if (this->m_statsEnabled) {
	this->m_statsRenders++;

	if (this->m_statsStart == std::chrono::steady_clock::time_point {}) {
	    this->m_statsStart = now;
	} else if (const double elapsed = std::chrono::duration<double> (now - this->m_statsStart).count (); elapsed >= 5.0) {
	    sLog.out (
		"CWeb host ", this->m_hostPid, ": ", this->m_statsRenders / elapsed, " renders/s, ",
		this->m_statsFrames / elapsed, " page frames/s, one page frame every ", this->m_frameRequestDivisor,
		" render(s)"
	    );
	    this->m_statsStart = now;
	    this->m_statsRenders = 0;
	    this->m_statsFrames = 0;
	}
    }

    if (++this->m_rendersSinceRequest < this->m_frameRequestDivisor) {
	return;
    }

    this->m_rendersSinceRequest = 0;
    this->m_shm->requestFrame ();
}

namespace {
// truncates to what fits, without leaving half of a multi byte character at the end
void copyText (char* target, std::size_t capacity, const std::string& text) {
    std::size_t length = std::min (text.size (), capacity - 1);

    while (length > 0 && length < text.size () && (static_cast<unsigned char> (text[length]) & 0xC0) == 0x80) {
	length--;
    }

    std::memcpy (target, text.data (), length);
    target[length] = '\0';
}

// MPRIS art URLs are percent encoded file:// URLs, the way they appear on the bus
std::string coverPathFromUrl (const std::optional<std::string>& url) {
    if (!url.has_value () || !url->starts_with ("file://")) {
	return "";
    }

    std::string path;

    for (std::size_t i = 7; i < url->size (); i++) {
	if ((*url)[i] == '%' && i + 2 < url->size () && std::isxdigit ((*url)[i + 1]) && std::isxdigit ((*url)[i + 2])) {
	    path += static_cast<char> (std::stoi (url->substr (i + 1, 2), nullptr, 16));
	    i += 2;
	} else {
	    path += (*url)[i];
	}
    }

    return path;
}
} // namespace

void CWeb::updateMedia () {
    const auto& info = this->getContext ().getMediaSource ().getMediaInfo ();
    const auto& last = this->m_lastMedia;

    if (this->m_mediaPublished && info.title == last.title && info.artist == last.artist && info.album == last.album
	&& info.url == last.url && info.playbackState == last.playbackState && info.available == last.available
	&& info.duration == last.duration && info.position == last.position) {
	return;
    }

    const bool coverChanged = !this->m_mediaPublished || info.url != last.url;
    const std::string coverPath = coverPathFromUrl (info.url);
    Media::ThumbnailPalette palette;

    if (coverChanged) {
	this->m_coverVersion++;

	if (!coverPath.empty ()) {
	    palette = Media::loadThumbnailPalette (coverPath);
	}
    }

    auto& shm = *this->m_shm;
    const uint32_t seq = shm.mediaSeq.load (std::memory_order_relaxed);

    shm.mediaSeq.store (seq + 1, std::memory_order_release);
    std::atomic_thread_fence (std::memory_order_seq_cst);

    shm.mediaState = static_cast<int32_t> (info.playbackState);
    shm.mediaAvailable = info.available;
    // MPRIS reports microseconds, pages are given seconds
    shm.mediaPosition = info.position / 1000000.0;
    shm.mediaDuration = info.duration / 1000000.0;
    copyText (shm.mediaTitle, WebHostSharedMemory::MEDIA_TEXT, info.title);
    copyText (shm.mediaArtist, WebHostSharedMemory::MEDIA_TEXT, info.artist);
    copyText (shm.mediaAlbum, WebHostSharedMemory::MEDIA_TEXT, info.album);

    if (coverChanged) {
	shm.mediaCoverVersion = this->m_coverVersion;
	copyText (shm.mediaCoverPath, WebHostSharedMemory::MEDIA_PATH, coverPath);

	const glm::vec3 colors[5]
	    = { palette.primary, palette.secondary, palette.tertiary, palette.text, palette.highContrast };

	for (int i = 0; i < 5; i++) {
	    for (int channel = 0; channel < 3; channel++) {
		shm.mediaPalette[i][channel]
		    = static_cast<uint8_t> (std::clamp (colors[i][channel], 0.0f, 1.0f) * 255.0f + 0.5f);
	    }
	}
    }

    std::atomic_thread_fence (std::memory_order_seq_cst);
    shm.mediaSeq.store (seq + 2, std::memory_order_release);

    this->m_lastMedia = info;
    this->m_mediaPublished = true;
}

void CWeb::updateMouse (const glm::ivec4& viewport) {
    auto& input = this->getContext ().getInputContext ().getMouseInput ();

    const glm::dvec2 position = input.position ();

    // Convert from OpenGL coordinates (Y=0 at bottom) to CEF coordinates (Y=0 at top). Written
    // unconditionally every frame - cheap atomic stores, and the host process itself only forwards
    // clicks to CEF when it observes the value actually changed.
    const int x = std::clamp (static_cast<int> (position.x - viewport.x), 0, viewport.z);
    const int y = viewport.w - std::clamp (static_cast<int> (position.y - viewport.y), 0, viewport.w);

    this->m_shm->mouseX.store (static_cast<double> (x), std::memory_order_relaxed);
    this->m_shm->mouseY.store (static_cast<double> (y), std::memory_order_relaxed);
    this->m_shm->leftClick.store (static_cast<int32_t> (input.leftClick ()), std::memory_order_relaxed);
    this->m_shm->rightClick.store (static_cast<int32_t> (input.rightClick ()), std::memory_order_relaxed);
}

CWeb::~CWeb () {
    // waits for a callback that's running right now, so the shared memory can be unmapped safely below
    this->getAudioContext ().getRecorder ().removeSpectrumListener (this->m_spectrumListenerId);

    this->m_shm->quitRequested.store (true, std::memory_order_release);

    // Give the host process a chance to close its CEF browser and shut down cleanly before falling
    // back to killing it outright - same bounded-wait philosophy this used when CEF was embedded
    // directly in this process (500 iterations there too, just pumping CEF's own message loop
    // instead of polling waitpid - there's no message loop to pump here anymore).
    bool exited = false;
    const auto shutdownStart = std::chrono::steady_clock::now ();

    for (int i = 0; i < 500; i++) {
	int status = 0;

	if (waitpid (this->m_hostPid, &status, WNOHANG) == this->m_hostPid) {
	    exited = true;
	    break;
	}

	std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    if (this->m_statsEnabled) {
	sLog.out (
	    "CWeb host ", this->m_hostPid, exited ? " exited after " : " had to be killed after ",
	    std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - shutdownStart).count (),
	    "ms"
	);
    }

    if (!exited) {
	sLog.error ("CWeb: web host ", this->m_hostPid, " did not shut down in time, killing it");
	kill (this->m_hostPid, SIGKILL);
	waitpid (this->m_hostPid, nullptr, 0);
    }

    closeSharedMemory (this->m_shm, this->m_shmName, this->m_shmMaxWidth, this->m_shmMaxHeight, true);
}
