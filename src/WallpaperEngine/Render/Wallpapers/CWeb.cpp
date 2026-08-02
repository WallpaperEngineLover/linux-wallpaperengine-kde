// This code is a modification of the original projects that can be found at
// https://github.com/if1live/cef-gl-example
// https://github.com/andmcgregor/cefgui
#include "CWeb.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

#include <chrono>
#include <csignal>
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

    // ensure the viewport matches the window size, and resize if needed
    if (viewport.z != this->getWidth () || viewport.w != this->getHeight ()) {
	this->setSize (viewport.z, viewport.w);
    }

    // ensure the virtual mouse position is up to date
    this->updateMouse (viewport);
    // use the scene's framebuffer by default
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    // ensure we render over the whole framebuffer
    glViewport (0, 0, this->getWidth (), this->getHeight ());

    // Pull the latest frame the host process painted, if there's one we haven't uploaded yet.
    // Seqlock: frameSeq is even and stable while no write is in progress: odd means the host is
    // mid-write, and a value that changed across our read means we caught a write in progress and
    // may have read a torn frame - either way, just try again on a later call rather than block,
    // bounded so a host that's somehow writing every few microseconds can't stall rendering here.
    for (int attempt = 0; attempt < 4; attempt++) {
	const uint32_t seqBefore = this->m_shm->frameSeq.load (std::memory_order_acquire);

	if (seqBefore % 2 != 0 || seqBefore == this->m_lastUploadedSeq) {
	    break;
	}

	const uint32_t frameWidth = this->m_shm->frameWidth.load (std::memory_order_relaxed);
	const uint32_t frameHeight = this->m_shm->frameHeight.load (std::memory_order_relaxed);

	if (frameWidth == 0 || frameHeight == 0) {
	    break;
	}

	glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());

	// Only reallocate GPU storage (glTexImage2D) when the size actually changed - the common
	// case is the same size every frame, and glTexSubImage2D updating pixels in place avoids the
	// driver-side realloc/sync that a fresh glTexImage2D call triggers, up to 60 times a second.
	if (frameWidth == this->m_uploadedTextureWidth && frameHeight == this->m_uploadedTextureHeight) {
	    glTexSubImage2D (
		GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei> (frameWidth), static_cast<GLsizei> (frameHeight),
		GL_BGRA_EXT, GL_UNSIGNED_BYTE, this->m_shm->frameBuffer ()
	    );
	} else {
	    glTexImage2D (
		GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei> (frameWidth), static_cast<GLsizei> (frameHeight), 0,
		GL_BGRA_EXT, GL_UNSIGNED_BYTE, this->m_shm->frameBuffer ()
	    );
	    this->m_uploadedTextureWidth = frameWidth;
	    this->m_uploadedTextureHeight = frameHeight;
	}

	glBindTexture (GL_TEXTURE_2D, 0);

	const uint32_t seqAfter = this->m_shm->frameSeq.load (std::memory_order_acquire);

	if (seqAfter == seqBefore) {
	    this->m_lastUploadedSeq = seqAfter;
	    break;
	}
	// torn read, the host wrote a new frame while we were copying - retry
    }
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
    this->m_shm->quitRequested.store (true, std::memory_order_release);

    // Give the host process a chance to close its CEF browser and shut down cleanly before falling
    // back to killing it outright - same bounded-wait philosophy this used when CEF was embedded
    // directly in this process (500 iterations there too, just pumping CEF's own message loop
    // instead of polling waitpid - there's no message loop to pump here anymore).
    bool exited = false;

    for (int i = 0; i < 500; i++) {
	int status = 0;

	if (waitpid (this->m_hostPid, &status, WNOHANG) == this->m_hostPid) {
	    exited = true;
	    break;
	}

	std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    if (!exited) {
	kill (this->m_hostPid, SIGKILL);
	waitpid (this->m_hostPid, nullptr, 0);
    }

    closeSharedMemory (this->m_shm, this->m_shmName, this->m_shmMaxWidth, this->m_shmMaxHeight, true);
}
