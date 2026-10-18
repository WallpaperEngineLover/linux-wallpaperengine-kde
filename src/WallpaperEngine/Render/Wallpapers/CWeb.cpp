// This code is a modification of the original projects that can be found at
// https://github.com/if1live/cef-gl-example
// https://github.com/andmcgregor/cefgui
#include "CWeb.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Audio/Drivers/PulseProcessVolume.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Media/ThumbnailPalette.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/WebBrowser/CEF/TestHookPolicy.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;
using namespace WallpaperEngine::WebBrowser::IPC;

namespace {
// closes every fd but stdio so CEF doesn't inherit our Wayland/EGL/X11/audio connections; raw syscalls only, other
// threads may hold malloc locks
void closeInheritedFds () {
#if defined(SYS_close_range)
    syscall (SYS_close_range, 3u, ~0u, 0u);
#endif
}

// a page paints its background long before its content, so a flat color doesn't count. Samples a grid
bool frameHasContent (const uint8_t* pixels, uint32_t width, uint32_t height) {
    constexpr uint32_t columns = 48;
    constexpr uint32_t rows = 27;
    constexpr int tolerance = 2;
    const uint8_t* first = pixels;

    for (uint32_t row = 0; row < rows; row++) {
	const uint32_t y = (height - 1) * row / (rows - 1);

	for (uint32_t column = 0; column < columns; column++) {
	    const uint32_t x = (width - 1) * column / (columns - 1);
	    const uint8_t* pixel = pixels + (static_cast<size_t> (y) * width + x) * 4;

	    for (int channel = 0; channel < 4; channel++) {
		if (std::abs (pixel[channel] - first[channel]) > tolerance) {
		    return true;
		}
	    }
	}
    }

    return false;
}

// after a reinstall /proc/self/exe reads "<path> (deleted)" and CEF aborts resolving its zygote through it; the new
// binary at the original path is fine
std::string hostExecutable () {
    constexpr std::string_view deletedSuffix = " (deleted)";
    std::error_code error;
    const std::string target = std::filesystem::read_symlink ("/proc/self/exe", error).string ();

    if (error || !target.ends_with (deletedSuffix)) {
	return "/proc/self/exe";
    }

    std::string original = target.substr (0, target.size () - deletedSuffix.size ());

    if (access (original.c_str (), X_OK) != 0) {
	sLog.error ("CWeb: engine executable ", original, " was removed, the web host will likely fail to start");
	return "/proc/self/exe";
    }

    sLog.out ("CWeb: engine executable was replaced on disk, starting the web host from ", original);
    return original;
}
}

CWeb::CWeb (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const std::filesystem::path& resolvedBackgroundPath, const WallpaperState::TextureUVsScaling& scalingMode,
    const uint32_t& clampMode, const glm::ivec2& maxRenderSize
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode) {
    this->setupFramebuffers ();

    // shared frame capacity from maxRenderSize, the full output size would oversize separate wallpapers
    this->m_shmMaxWidth = static_cast<uint32_t> (std::max (1, maxRenderSize.x));
    this->m_shmMaxHeight = static_cast<uint32_t> (std::max (1, maxRenderSize.y));

    this->m_shm = createSharedMemory (this->m_shmName, this->m_shmMaxWidth, this->m_shmMaxHeight);

    if (this->m_shm == nullptr) {
	sLog.exception ("CWeb: failed to create shared memory for the web host process");
    }

    this->m_width = static_cast<int> (this->m_shmMaxWidth);
    this->m_height = static_cast<int> (this->m_shmMaxHeight);
    this->m_shm->desiredWidth.store (static_cast<uint32_t> (this->m_width), std::memory_order_relaxed);
    this->m_shm->desiredHeight.store (static_cast<uint32_t> (this->m_height), std::memory_order_relaxed);

    // pushed from the recorder's capture thread, the host process only ever reads these atomics
    auto* shm = this->m_shm;

    this->m_spectrumRecorder = &this->getAudioContext ().getRecorder ();
    this->m_spectrumListenerId = this->m_spectrumRecorder->addSpectrumListener ([shm] (const float* bands) {
	for (std::size_t i = 0; i < WebHostSharedMemory::AUDIO_BANDS; i++) {
	    shm->audioBands[i].store (bands[i], std::memory_order_relaxed);
	}
    });

    this->m_statsEnabled = std::getenv ("LWE_WEB_STATS") != nullptr;

    this->spawnHost (resolvedBackgroundPath);

    this->m_streamVolume = std::make_unique<Audio::Drivers::PulseProcessVolume> (this->m_hostPid);
    this->setAudioPolicy (false, std::nullopt);
}

void CWeb::spawnHost (const std::filesystem::path& resolvedBackgroundPath) {
    auto& appContext = this->getContext ().getApp ().getContext ();
    const std::string executable = hostExecutable ();

    // stream restore would hand our "Chromium" volume to the user's browser, so the host's stream opts out
    // tests only (HEADLESS_RENDER_WEB=1): LD_PRELOAD for the host alone, libcef.so before libc for its close() wrapper
    const char* preload = std::getenv ("LWE_WEB_HOST_PRELOAD");

    if (preload != nullptr && preload[0] != '\0' && !WallpaperEngine::WebBrowser::CEF::trustedPreloadFile (preload)) {
	sLog.error (
	    "LWE_WEB_HOST_PRELOAD: ignoring ", preload, ", it must be an absolute path to a file only you can write"
	);
	preload = nullptr;
    }

    std::vector<std::string> environment;

    for (char** entry = environ; *entry != nullptr; entry++) {
	const std::string_view variable (*entry);

	if (!variable.starts_with ("PULSE_PROP_state.restore-props=")
	    && !variable.starts_with ("PULSE_PROP_module-stream-restore.id=")
	    && !(preload != nullptr && preload[0] != '\0' && variable.starts_with ("LD_PRELOAD="))) {
	    environment.emplace_back (*entry);
	}
    }

    environment.emplace_back ("PULSE_PROP_state.restore-props=false");
    environment.emplace_back ("PULSE_PROP_module-stream-restore.id=lwe-web");

    if (preload != nullptr && preload[0] != '\0') {
	environment.emplace_back (std::string ("LD_PRELOAD=") + preload);
    }

    std::vector<char*> envp;

    for (auto& entry : environment) {
	envp.push_back (entry.data ());
    }

    envp.push_back (nullptr);

    const pid_t pid = fork ();

    if (pid < 0) {
	// sLog.exception() throws before ~CWeb() runs, unlink the shm segment here
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
	// cosmetic argv[0], the executable run is hostExecutable()
	argv.push_back (appContext.getArgv ()[0]);

	for (auto& arg : args) {
	    argv.push_back (arg.data ());
	}

	argv.push_back (nullptr);

	execve (executable.c_str (), argv.data (), envp.data ());

	_exit (127);
    }

    this->m_hostPid = pid;
}

void CWeb::setAudioPolicy (bool muted, std::optional<int> ambientVolume) {
    if (this->m_shm == nullptr) {
	return;
    }

    const auto& appContext = this->getContext ().getApp ().getContext ();
    const int volume
	= muted || !appContext.settings.audio.enabled ? 0 : ambientVolume.value_or (appContext.state.audio.volume);

    this->m_shm->audioMuted.store (volume == 0, std::memory_order_relaxed);

    if (this->m_streamVolume) {
	this->m_streamVolume->setVolume (static_cast<float> (volume) / 128.0f);
    }
}

void CWeb::setSize (const int width, const int height) {
    this->m_width = width > 0 ? width : this->m_width;
    this->m_height = height > 0 ? height : this->m_height;

    if (this->m_width <= 0 || this->m_height <= 0) {
	return;
    }

    // clamped to the capacity, the host notices and calls WasResized()
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

    this->updateMouse (this->cursorViewport ());
    this->updateMedia ();
    this->m_streamVolume->update ();
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    glViewport (0, 0, this->getWidth (), this->getHeight ());

    const auto taken = this->m_shm->takeFrame (this->m_frontSlot);

    if (!taken.has_value ()) {
	this->requestPageFrame ();
	return;
    }

    if (*taken >= WebHostSharedMemory::FRAME_SLOTS) {
	this->rejectFrame ();
	return;
    }

    this->m_frontSlot = *taken;

    const uint32_t frameWidth = this->m_shm->slotWidth[this->m_frontSlot].load (std::memory_order_relaxed);
    const uint32_t frameHeight = this->m_shm->slotHeight[this->m_frontSlot].load (std::memory_order_relaxed);

    if (frameWidth == 0 || frameHeight == 0) {
	this->requestPageFrame ();
	return;
    }

    if (!WebHostSharedMemory::frameFits (
	    this->m_frontSlot, frameWidth, frameHeight, this->m_shmMaxWidth, this->m_shmMaxHeight
	)) {
	this->rejectFrame ();
	return;
    }

    uint8_t* const pixels = this->m_shm->frameBuffer (this->m_frontSlot, this->m_shmMaxWidth, this->m_shmMaxHeight);

    if (frameWidth != this->m_uploadedTextureWidth || frameHeight != this->m_uploadedTextureHeight) {
	this->find ("_rt_FullFrameBuffer")->resize (frameWidth, frameHeight);
	this->m_uploadedTextureWidth = frameWidth;
	this->m_uploadedTextureHeight = frameHeight;
    }

    glBindTexture (GL_TEXTURE_2D, this->getWallpaperTexture ());
    glTexSubImage2D (
	GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei> (frameWidth), static_cast<GLsizei> (frameHeight), GL_BGRA_EXT,
	GL_UNSIGNED_BYTE, pixels
    );
    glBindTexture (GL_TEXTURE_2D, 0);

    if (!this->m_pageHasContent && frameHasContent (pixels, frameWidth, frameHeight)) {
	this->m_pageHasContent = true;

	if (this->m_statsEnabled) {
	    sLog.out (
		"CWeb host ", this->m_hostPid, ": first page content after ",
		std::chrono::duration_cast<std::chrono::milliseconds> (
		    std::chrono::steady_clock::now () - this->m_created
		)
		    .count (),
		"ms"
	    );
	}
    }

    this->m_statsFrames++;

    // ask for the next frame only after taking this one, so what gets painted is always what the next render shows
    this->requestPageFrame ();
}

bool CWeb::hasContent () const {
    constexpr auto patience = std::chrono::seconds (10);

    return this->m_pageHasContent || this->m_shm->helperFailed.load (std::memory_order_relaxed)
	|| std::chrono::steady_clock::now () - this->m_created > patience;
}

void CWeb::rejectFrame () {
    if (!this->m_badFrameLogged) {
	sLog.error ("CWeb: the host published a frame that does not fit its shared memory, ignoring it");
	this->m_badFrameLogged = true;
    }

    this->requestPageFrame ();
}

void CWeb::requestPageFrame () {
    const auto now = std::chrono::steady_clock::now ();

    if (this->m_lastRender != std::chrono::steady_clock::time_point {}) {
	const double interval = std::chrono::duration<double> (now - this->m_lastRender).count ();

	if (interval < 0.1) {
	    this->m_renderInterval
		= this->m_renderInterval == 0.0 ? interval : this->m_renderInterval * 0.95 + interval * 0.05;

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
	} else if (
	    const double elapsed = std::chrono::duration<double> (now - this->m_statsStart).count (); elapsed >= 5.0
	) {
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
}

void CWeb::updateMedia () {
    const auto& info = this->getContext ().getMediaSource ().getMediaInfo ();
    const auto& last = this->m_lastMedia;

    if (this->m_mediaPublished && info.title == last.title && info.artist == last.artist && info.album == last.album
	&& info.cover == last.cover && info.playbackState == last.playbackState && info.available == last.available
	&& info.duration == last.duration && info.position == last.position) {
	return;
    }

    const bool coverChanged = !this->m_mediaPublished || info.cover != last.cover;
    const std::string coverPath = info.cover.has_value () ? info.cover->string () : "";
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
    const glm::dvec2 position = this->cursorPosition ();

    // GL y up to CEF y down; written every frame, the host only forwards clicks when the value changes
    const int x = std::clamp (static_cast<int> (position.x - viewport.x), 0, viewport.z);
    const int y = viewport.w - std::clamp (static_cast<int> (position.y - viewport.y), 0, viewport.w);

    this->m_shm->mouseX.store (static_cast<double> (x), std::memory_order_relaxed);
    this->m_shm->mouseY.store (static_cast<double> (y), std::memory_order_relaxed);
    this->m_shm->leftClick.store (static_cast<int32_t> (this->cursorLeftClick ()), std::memory_order_relaxed);
    this->m_shm->rightClick.store (static_cast<int32_t> (this->cursorRightClick ()), std::memory_order_relaxed);
}

CWeb::~CWeb () {
    // waits for a callback that's running right now, so the shared memory can be unmapped safely below
    this->m_spectrumRecorder->removeSpectrumListener (this->m_spectrumListenerId);

    this->m_shm->quitRequested.store (true, std::memory_order_release);

    // let the host close CEF cleanly before killing it, bounded like the old embedded loop
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
	    std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - shutdownStart)
		.count (),
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

void CWeb::setPropertyOverrides (const std::map<std::string, std::string>& overrides) {
    if (this->m_shm == nullptr) {
	return;
    }

    std::string text;

    for (const auto& [name, value] : overrides) {
	text += name + "=" + value + "\n";
    }

    if (text.size () >= WebHostSharedMemory::PROPERTIES_TEXT) {
	sLog.error (
	    "Property overrides too long for the web host (", text.size (), " bytes), the page keeps its values"
	);
	return;
    }

    auto& shm = *this->m_shm;
    const uint32_t seq = shm.propertiesSeq.load (std::memory_order_relaxed);

    shm.propertiesSeq.store (seq + 1, std::memory_order_release);
    std::atomic_thread_fence (std::memory_order_seq_cst);
    copyText (shm.propertiesText, WebHostSharedMemory::PROPERTIES_TEXT, text);
    std::atomic_thread_fence (std::memory_order_seq_cst);
    shm.propertiesSeq.store (seq + 2, std::memory_order_release);
}
