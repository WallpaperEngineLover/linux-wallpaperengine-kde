#include "GLPlayer.h"

#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace WallpaperEngine::VideoPlayback;
using namespace WallpaperEngine::VideoPlayback::MPV;

std::unordered_map<std::filesystem::path, GLPlayer*> GLPlayer::s_activePlayers;
std::unordered_set<GLPlayer*> GLPlayer::s_players;
double GLPlayer::s_statsMillis = 0.0;
uint64_t GLPlayer::s_frame = 0;
bool GLPlayer::s_globalPause = false;

// wallpaper64.exe 2.8.42 sub_1400F3930: the media engine plays while (+96 + 30) >= frame counter, +96 being the
// counter at the last transfer
static constexpr uint64_t UNUSED_FRAMES_BEFORE_PAUSE = 30;

// LWE_FIXED_TIMESTEP (regression renders): the video is paused and every scene frame seeks it to that frame's time, so
// a screenshot shows the same video frame on every run
static double fixedStep () {
    static const double step = [] {
	const char* value = std::getenv ("LWE_FIXED_TIMESTEP");
	return value != nullptr ? std::max (0.0, std::strtod (value, nullptr)) : 0.0;
    }();

    return step;
}

// mpv leaves a PBO and row settings bound, later glTexImage2D calls expect GL's defaults
static void resetUnpackState () {
    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
}

// the synchronous mpv_set_property/mpv_command wait for mpv's core thread, which can take a few hundred ms
// while it's busy, and these are called from the render thread (scripts pausing/seeking videos on click)
static void setPropertyAsync (mpv_handle* handle, const char* name, const char* value) {
    mpv_set_property_async (handle, 0, name, MPV_FORMAT_STRING, &value);
}

void* get_proc_address (void* ctx, const char* name) {
    return static_cast<GLPlayer*> (ctx)->getContext ().getDriver ().getProcAddress (name);
}

GLPlayer::GLPlayer (
    RenderContext& context, GLuint outputTexture, const std::filesystem::path& file, const int64_t baseWidth,
    const int64_t baseHeight, const GLuint fbo
) : ContextAware (context), m_outputTexture (outputTexture), m_width (baseWidth), m_height (baseHeight) {
    this->m_fbo = fbo;
    this->m_doWeOwnFramebuffer = this->m_fbo == GL_NONE;

    this->prepareGL ();
    this->setSource (file);
    s_players.insert (this);
}

GLPlayer::GLPlayer (
    RenderContext& context, const GLuint outputTexture, MemoryStreamProtocolUniquePtr stream, const int64_t baseWidth,
    const int64_t baseHeight, const GLuint fbo
) : ContextAware (context), m_outputTexture (outputTexture), m_width (baseWidth), m_height (baseHeight) {
    this->m_fbo = fbo;
    this->m_doWeOwnFramebuffer = this->m_fbo == GL_NONE;

    this->prepareGL ();
    this->setSource (std::move (stream));
    s_players.insert (this);
}

GLPlayer::~GLPlayer () {
    s_players.erase (this);
    this->stop ();

    if (this->m_doWeOwnFramebuffer) {
	glDeleteFramebuffers (1, &this->m_fbo);
    }
}

void GLPlayer::incrementUsageCount () {
    this->m_usageCount++;

    if (this->m_usageCount == 1) {
	this->play ();
    }
}

void GLPlayer::decrementUsageCount () {
    if (this->m_usageCount == 0) {
	sLog.exception ("GLPlayer usage count would underflow");
    }

    this->m_usageCount--;

    if (this->m_usageCount == 0) {
	this->stop ();
    }
}

void GLPlayer::setUntimed () {
    if (this->m_handle) {
	sLog.exception ("Cannot set untimed mode after playback has started");
    }

    this->m_untimed = true;
}

void GLPlayer::disableAudio () {
    if (this->m_handle) {
	sLog.exception ("Cannot disable audio after playback has started");
    }

    this->m_audio = false;
}

void GLPlayer::setLinearOutput () { this->m_linearOutput = true; }

void GLPlayer::clearUntimed () {
    if (this->m_handle) {
	sLog.exception ("Cannot set untimed mode after playback has started");
    }

    this->m_untimed = false;
}

void GLPlayer::setMuted () {
    this->m_muted = true;

    if (this->m_handle) {
	setPropertyAsync (this->m_handle, "mute", "yes");
    }
}

void GLPlayer::clearMuted () {
    this->m_muted = false;

    if (this->m_handle) {
	setPropertyAsync (this->m_handle, "mute", "no");
    }
}

void GLPlayer::setVolume (double volume) {
    this->m_volume = volume;

    if (this->m_handle) {
	mpv_set_property_async (this->m_handle, 0, "volume", MPV_FORMAT_DOUBLE, &this->m_volume);
    }
}

void GLPlayer::setSpeed (double speed) {
    this->m_speed = speed;

    if (this->m_handle) {
	mpv_set_property_async (this->m_handle, 0, "speed", MPV_FORMAT_DOUBLE, &this->m_speed);
    }
}

bool GLPlayer::isValidHwdec (const std::string& value) {
    if (value.empty () || value.front () == ',' || value.back () == ',') {
	return false;
    }

    return std::all_of (value.begin (), value.end (), [] (const char c) {
	return std::isalnum (static_cast<unsigned char> (c)) || c == '-' || c == '_' || c == ',';
    });
}

void GLPlayer::setHwdecForAll (const std::string& value) {
    for (GLPlayer* player : s_players) {
	if (player->m_handle) {
	    setPropertyAsync (player->m_handle, "hwdec", value.c_str ());
	}
    }
}

void GLPlayer::setPaused () {
    this->m_paused = true;
    this->applyPause ();
}

void GLPlayer::clearPaused () {
    this->m_paused = false;
    this->applyPause ();
}

void GLPlayer::setGlobalPause (bool paused) {
    s_globalPause = paused;

    for (GLPlayer* player : s_players) {
	player->applyPause ();
    }
}

void GLPlayer::markUsed () const {
    this->m_lastUsedFrame = s_frame;

    // the frame counter only moves while a scene renders, a player paused for going unused can't wait for that
    if (this->m_enginePaused && !this->m_paused && !s_globalPause) {
	this->applyPause ();
    }
}

void GLPlayer::applyPause () const {
    const bool paused = this->m_paused || s_globalPause || fixedStep () > 0.0
	|| this->m_lastUsedFrame + UNUSED_FRAMES_BEFORE_PAUSE < s_frame;

    if (this->m_handle == nullptr || paused == this->m_enginePaused) {
	return;
    }

    this->m_enginePaused = paused;
    setPropertyAsync (this->m_handle, "pause", paused ? "yes" : "no");
}

void GLPlayer::advanceFrame () {
    s_frame++;

    for (GLPlayer* player : s_players) {
	player->applyPause ();
    }
}

void GLPlayer::setLoop (bool loop) {
    this->m_loop = loop;

    if (this->m_handle) {
	setPropertyAsync (this->m_handle, "loop", loop ? "inf" : "no");
    }
}

// eof-reached uses 0
static constexpr uint64_t TIME_POS_OBSERVER = 1;
static constexpr uint64_t FPS_OBSERVER = 2;
static constexpr uint64_t POSITION_OBSERVER = 3;
static constexpr uint64_t WIDTH_OBSERVER = 4;
static constexpr uint64_t HEIGHT_OBSERVER = 5;

// with ab-loop-b unset mpv's own loop restarts the file from 0 instead of going back to A, so an open end is a B
// past the end of any video
static std::pair<std::string, std::string> abLoopPoints (const VideoSegments& segments) {
    if (segments.empty ()) {
	return { "no", "no" };
    }

    const auto& end = segments.back ().end;

    return { std::to_string (segments.front ().start), end.has_value () ? std::to_string (*end) : "1e9" };
}

void GLPlayer::setSegments (const VideoSegments& segments) {
    // re-sending the same parts must not interrupt playback
    if (this->m_handle != nullptr && segments == this->m_segments) {
	return;
    }

    this->m_segments = segments;

    if (this->m_handle == nullptr) {
	return;
    }

    if (this->m_fileLoaded) {
	this->dropSegmentsPastEnd ();
    }

    const auto [a, b] = abLoopPoints (this->m_segments);

    setPropertyAsync (this->m_handle, "ab-loop-a", a.c_str ());
    setPropertyAsync (this->m_handle, "ab-loop-b", b.c_str ());
    this->observePosition ();
    this->seek (this->m_segments.empty () ? 0.0 : this->m_segments.front ().start);
}

bool GLPlayer::dropSegmentsPastEnd () const {
    const double duration = this->getDuration ();

    if (duration <= 0.0) {
	return false;
    }

    const auto dropped = std::erase_if (this->m_segments, [duration] (const VideoSegment& segment) {
	if (segment.start < duration) {
	    return false;
	}

	sLog.error (
	    "Video part from ", segment.start, "s starts past the end of the video (", duration, "s), skipping it"
	);
	return true;
    });

    if (dropped > 0 && this->m_segments.empty ()) {
	sLog.error ("No video part left, playing all of it");
    }

    return dropped > 0;
}

void GLPlayer::observePosition () const {
    mpv_unobserve_property (this->m_handle, TIME_POS_OBSERVER);
    mpv_unobserve_property (this->m_handle, FPS_OBSERVER);
    this->m_skipping = false;

    if (this->m_segments.size () > 1) {
	mpv_observe_property (this->m_handle, TIME_POS_OBSERVER, "time-pos", MPV_FORMAT_DOUBLE);
	mpv_observe_property (this->m_handle, FPS_OBSERVER, "container-fps", MPV_FORMAT_DOUBLE);
    }
}

std::optional<double> GLPlayer::skipTarget (const double position) const {
    // jump when the next frame would be past the end
    const double next = position + this->m_frameDuration * 0.99;

    for (const auto& segment : this->m_segments) {
	if (position < segment.start) {
	    return segment.start;
	}

	if (!segment.end.has_value () || next < *segment.end) {
	    return std::nullopt;
	}
    }

    // past the last part the A-B loop goes back to the first
    return std::nullopt;
}

void GLPlayer::seek (const double seconds) {
    this->m_ended = false;
    this->m_position = seconds;

    if (this->m_handle == nullptr || !this->m_fileLoaded) {
	this->m_pendingSeek = seconds;
	return;
    }

    const std::string target = std::to_string (seconds);
    const char* command[] = { "seek", target.c_str (), "absolute+exact", nullptr };

    mpv_command_async (this->m_handle, 0, command);
}

double GLPlayer::getDuration () const {
    double duration = 0.0;

    if (this->m_handle != nullptr) {
	mpv_get_property (this->m_handle, "duration", MPV_FORMAT_DOUBLE, &duration);
    }

    return duration;
}

void GLPlayer::applyDisplaySize () const {
    if (this->m_displayWidth <= 0 || this->m_displayHeight <= 0
	|| (this->m_displayWidth == this->m_width && this->m_displayHeight == this->m_height)) {
	return;
    }

    this->m_width = this->m_displayWidth;
    this->m_height = this->m_displayHeight;
    this->m_needsRedraw = true;
    glBindTexture (GL_TEXTURE_2D, this->m_outputTexture);
    glTexImage2D (
	GL_TEXTURE_2D, 0, this->m_linearOutput ? GL_RGBA16F : GL_RGBA8, this->m_width, this->m_height, 0, GL_RGBA,
	this->m_linearOutput ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr
    );

    // stays empty like WE's zero filled one (sub_1400EB570) until a frame is drawn below, a paused texture gets
    // none
    if (this->m_doWeOwnFramebuffer) {
	this->clearTexture ();
    }
}

bool GLPlayer::pumpEvents (double timeout) const {
    bool restarted = false;

    while (true) {
	const mpv_event* event = mpv_wait_event (this->m_handle, timeout);

	timeout = 0.0;

	if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
	    break;
	}

	if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
	    this->m_skipping = false;
	    restarted = true;
	    continue;
	}

	if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
	    const auto* property = static_cast<const mpv_event_property*> (event->data);

	    if (event->reply_userdata == POSITION_OBSERVER) {
		if (property->format == MPV_FORMAT_DOUBLE) {
		    this->m_position = *static_cast<const double*> (property->data);
		}

		continue;
	    }

	    if (event->reply_userdata == WIDTH_OBSERVER || event->reply_userdata == HEIGHT_OBSERVER) {
		const int64_t value
		    = property->format == MPV_FORMAT_INT64 ? *static_cast<const int64_t*> (property->data) : 0;

		(event->reply_userdata == WIDTH_OBSERVER ? this->m_displayWidth : this->m_displayHeight) = value;
		this->applyDisplaySize ();
		continue;
	    }

	    if (event->reply_userdata == FPS_OBSERVER) {
		const double fps
		    = property->format == MPV_FORMAT_DOUBLE ? *static_cast<const double*> (property->data) : 0.0;

		this->m_frameDuration = fps > 0.0 ? 1.0 / fps : 0.0;
		continue;
	    }

	    if (event->reply_userdata == TIME_POS_OBSERVER) {
		if (property->format != MPV_FORMAT_DOUBLE || this->m_skipping || !this->m_fileLoaded) {
		    continue;
		}

		if (const auto target = this->skipTarget (*static_cast<const double*> (property->data))) {
		    const std::string position = std::to_string (*target);
		    const char* command[] = { "seek", position.c_str (), "absolute+exact", nullptr };

		    this->m_skipping = true;
		    mpv_command_async (this->m_handle, 0, command);
		}

		continue;
	    }

	    if (property->format == MPV_FORMAT_FLAG && std::string_view (property->name) == "eof-reached"
		&& *static_cast<const int*> (property->data) && !this->m_loop) {
		this->m_ended = true;
	    }

	    continue;
	}

	if (event->event_id == MPV_EVENT_FILE_LOADED) {
	    this->m_fileLoaded = true;

	    if (this->dropSegmentsPastEnd ()) {
		const auto [a, b] = abLoopPoints (this->m_segments);

		setPropertyAsync (this->m_handle, "ab-loop-a", a.c_str ());
		setPropertyAsync (this->m_handle, "ab-loop-b", b.c_str ());
		this->observePosition ();
		this->m_pendingSeek = this->m_segments.empty () ? 0.0 : this->m_segments.front ().start;
	    }

	    if (this->m_pendingSeek.has_value ()) {
		const std::string target = std::to_string (*this->m_pendingSeek);
		const char* command[] = { "seek", target.c_str (), "absolute+exact", nullptr };

		this->m_pendingSeek.reset ();
		mpv_command_async (this->m_handle, 0, command);
	    }

	    continue;
	}
    }

    return restarted;
}

void GLPlayer::render () const {
    // only render while actively playing (m_handle is set by usage-count-driven play())
    if (this->m_handle == nullptr) {
	return;
    }

    const bool stepped = fixedStep () > 0.0 && this->m_segments.empty ();

    if (stepped) {
	const auto loadDeadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);

	while (!this->m_fileLoaded && std::chrono::steady_clock::now () < loadDeadline) {
	    this->pumpEvents (0.01);
	}
    }

    this->pumpEvents (0.0);

    // mpv only hands out its next frame once the update flags were collected, without this playback stalls
    uint64_t updateFlags = mpv_render_context_update (this->m_renderContext);

    if (stepped) {
	updateFlags |= this->stepToFrame ();
    }

    if (this->m_doWeOwnFramebuffer) {
	// WE's pause stops the thread that copies frames into the texture (sub_1400F3030), so a seek or stop while
	// paused keeps showing the last frame until play
	if (this->m_paused) {
	    return;
	}

	if (!(updateFlags & MPV_RENDER_UPDATE_FRAME) && !this->m_needsRedraw) {
	    return;
	}

	// before the first decoded frame mpv only queues redraws of nothing, which it draws black. WE's texture
	// keeps its zeros until the first TransferVideoFrame (sub_1400F3480). They still have to be consumed, or
	// mpv's VO waits 200 ms for each one
	if (!this->m_hasFrame) {
	    mpv_render_frame_info info {};
	    mpv_render_context_get_info (this->m_renderContext, { MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info });

	    if ((info.flags & MPV_RENDER_FRAME_INFO_PRESENT) && !(info.flags & MPV_RENDER_FRAME_INFO_REDRAW)) {
		this->m_hasFrame = true;
	    } else {
		int skip = 1;
		mpv_render_param params[]
		    = { { MPV_RENDER_PARAM_SKIP_RENDERING, &skip }, { MPV_RENDER_PARAM_INVALID, nullptr } };

		if (info.flags & MPV_RENDER_FRAME_INFO_PRESENT) {
		    mpv_render_context_render (this->m_renderContext, params);
		    resetUnpackState ();
		}

		return;
	    }
	}

	this->m_needsRedraw = false;
    } else if (this->m_renderOnlyNew) {
	if (!(updateFlags & MPV_RENDER_UPDATE_FRAME) && !this->m_needsRedraw) {
	    return;
	}

	this->m_needsRedraw = false;
    }

    // scene passes leave their own mask behind, and a frame drawn without alpha leaves the texture transparent
    struct FullColorMask {
	GLboolean saved[4] {};

	FullColorMask () {
	    glGetBooleanv (GL_COLOR_WRITEMASK, this->saved);
	    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}

	~FullColorMask () { glColorMask (this->saved[0], this->saved[1], this->saved[2], this->saved[3]); }
    } colorMask;

    glViewport (0, 0, this->m_width, this->m_height);

    mpv_opengl_fbo fbo { static_cast<int> (this->m_fbo), static_cast<int> (this->m_width),
			 static_cast<int> (this->m_height), this->m_linearOutput ? GL_RGBA16F : GL_RGBA8 };

    // flipped by the wallpaper rendering
    int flip_y = 0;
    // by default mpv sleeps in here until the frame's display time, stalling the whole scene
    // (~20ms of every 33ms frame per texture video). A texture is just sampled later, so don't wait
    int blockForTargetTime = this->m_doWeOwnFramebuffer ? 0 : 1;

    mpv_render_param params[] = { { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
				  { MPV_RENDER_PARAM_FLIP_Y, &flip_y },
				  { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime },
				  { MPV_RENDER_PARAM_INVALID, nullptr } };

    static const bool collectStats = std::getenv ("LWE_FRAME_STATS") != nullptr;

    if (!collectStats) {
	mpv_render_context_render (this->m_renderContext, params);
	resetUnpackState ();

	if (stepped) {
	    // one seek can queue more than one frame, the last of them is the sought one
	    using Clock = std::chrono::steady_clock;

	    // the first seeks of a file leave frames behind that arrive late
	    const auto settle = std::chrono::milliseconds (this->m_stepCount++ < 3 ? 300 : 30);

	    for (auto quiet = Clock::now () + settle; Clock::now () < quiet;) {
		if (mpv_render_context_update (this->m_renderContext) & MPV_RENDER_UPDATE_FRAME) {
		    mpv_render_frame_info info {};
		    mpv_render_context_get_info (this->m_renderContext, { MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info });

		    if ((info.flags & MPV_RENDER_FRAME_INFO_PRESENT) && !(info.flags & MPV_RENDER_FRAME_INFO_REDRAW)) {
			mpv_render_context_render (this->m_renderContext, params);
			resetUnpackState ();
			quiet = Clock::now () + settle;
		    }
		}

		std::this_thread::sleep_for (std::chrono::milliseconds (1));
	    }
	}

	return;
    }

    glFinish ();
    const auto start = std::chrono::steady_clock::now ();
    mpv_render_context_render (this->m_renderContext, params);
    resetUnpackState ();
    glFinish ();
    s_statsMillis += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
}

uint64_t GLPlayer::stepToFrame () const {
    if (!this->m_fileLoaded || this->m_paused) {
	return 0;
    }

    double target = static_cast<double> (s_frame - this->m_stepOrigin) * fixedStep () * this->m_speed;

    if (const double duration = this->getDuration (); duration > 0.0) {
	target = this->m_loop ? std::fmod (target, duration) : std::min (target, duration);
    }

    const bool first = this->m_steppedTo < 0.0;

    if (std::abs (target - std::max (this->m_steppedTo, 0.0)) < 1e-6 && !first) {
	return 0;
    }

    uint64_t flags = 0;

    if (!first || target > 1e-6) {
	// events of an earlier seek must not pass for this one's
	this->pumpEvents (0.0);
	mpv_render_context_update (this->m_renderContext);

	const std::string position = std::to_string (target);
	const char* command[] = { "seek", position.c_str (), "absolute+exact", nullptr };

	mpv_command (this->m_handle, command);

	const double tolerance = std::max (this->m_frameDuration, 0.05);
	const auto landDeadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
	bool restarted = false;

	while (std::chrono::steady_clock::now () < landDeadline) {
	    restarted |= this->pumpEvents (0.005);

	    if (restarted && std::abs (this->m_position - target) <= tolerance) {
		break;
	    }
	}
    }

    this->m_steppedTo = target;

    double barrier = 0.0;

    mpv_get_property (this->m_handle, "time-pos", MPV_FORMAT_DOUBLE, &barrier);

    // a redraw of the old picture raises the flag too, the sought frame is the one that is not a redraw
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);

    while (std::chrono::steady_clock::now () < deadline) {
	flags |= mpv_render_context_update (this->m_renderContext);

	mpv_render_frame_info info {};
	mpv_render_context_get_info (this->m_renderContext, { MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info });

	if ((flags & MPV_RENDER_UPDATE_FRAME) && (info.flags & MPV_RENDER_FRAME_INFO_PRESENT)
	    && !(info.flags & MPV_RENDER_FRAME_INFO_REDRAW)) {
	    break;
	}

	std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    return flags;
}

int GLPlayer::getWidth () const { return this->m_width; }
int GLPlayer::getHeight () const { return this->m_height; }

void GLPlayer::prepareGL () {
    if (!this->m_doWeOwnFramebuffer || this->m_fbo != GL_NONE) {
	return;
    }

    glGenFramebuffers (1, &this->m_fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_fbo);
    glBindTexture (GL_TEXTURE_2D, this->m_outputTexture);
    glTexImage2D (
	GL_TEXTURE_2D, 0, this->m_linearOutput ? GL_RGBA16F : GL_RGBA8, this->m_width, this->m_height, 0, GL_RGBA,
	this->m_linearOutput ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr
    );
    constexpr GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_outputTexture, 0);
    glDrawBuffers (1, drawBuffers);

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.exception ("Framebuffers are not properly set");
    }

    this->clearTexture ();
}

void GLPlayer::clearTexture () const {
    constexpr GLfloat transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_fbo);
    glClearBufferfv (GL_COLOR, 0, transparent);
}

void GLPlayer::init () {
    this->m_fileLoaded = false;
    this->m_hasFrame = false;
    this->m_ended = false;
    this->m_skipping = false;
    this->m_position = 0.0;
    this->m_stepOrigin = s_frame;
    this->m_steppedTo = -1.0;
    this->m_handle = mpv_create ();

    if (this->m_handle == nullptr) {
	sLog.exception ("Cannot create mpv context for video texture");
    }

    mpv_set_option_string (this->m_handle, "terminal", "yes");
#if NDEBUG
    mpv_set_option_string (this->m_handle, "msg-level", "all=status,statusline=no");
#else
    mpv_set_option_string (this->m_handle, "msg-level", "all=v");
#endif
    mpv_set_option_string (this->m_handle, "input-cursor", "no");
    mpv_set_option_string (this->m_handle, "cursor-autohide", "no");
    mpv_set_option_string (this->m_handle, "config", "no");
    // a wallpaper's video is a plain file: neither of these alone stops an .mp4 that is really an M3U, EDL or HLS
    // playlist from making mpv fetch URLs, the second one then hands them to yt-dlp
    mpv_set_option_string (this->m_handle, "access-references", "no");
    mpv_set_option_string (this->m_handle, "ytdl", "no");
    mpv_set_option_string (this->m_handle, "fbo-format", this->m_linearOutput ? "rgba16f" : "rgba8");

    if (this->m_linearOutput) {
	mpv_set_option_string (this->m_handle, "target-trc", "linear");
	mpv_set_option_string (this->m_handle, "target-prim", "bt.2020");
	// linear's nominal peak is reference white, mpv would tone map everything down to SDR; the compositor
	// maps PQ to what the monitor can show
	mpv_set_option_string (this->m_handle, "target-peak", "10000");
    }
    mpv_set_option_string (this->m_handle, "vo", "libmpv");
    mpv_set_option_string (this->m_handle, "profile", "fast");
    mpv_set_option_string (this->m_handle, "untimed", this->m_untimed ? "yes" : "no");
    if (!this->m_audio) {
	mpv_set_option_string (this->m_handle, "aid", "no");
    }
    if (this->m_doWeOwnFramebuffer) {
	// without the blocking wait in render() a frame would otherwise show up to 50ms early
	mpv_set_option_string (this->m_handle, "video-timing-offset", "0");
	// after every seek, the loop's included, mpv holds its clock until the scene has drawn the first new frame,
	// which lost 1-3 frames per loop at 15 fps. Media Foundation's loop in live WE on Windows loses ~0.3
	mpv_set_option_string (this->m_handle, "video-latency-hacks", "yes");
    }

    if (mpv_initialize (this->m_handle) < 0) {
	sLog.exception ("Could not initialize mpv context");
    }

    // keep-open parks a finished video on its last frame instead of unloading it, so a script can still seek it
    // back to the start. Observed rather than polled: mpv_get_property waits for mpv's core thread, and polling
    // it every frame stalled the render thread ~160-200ms every few seconds on a paused video
    mpv_observe_property (this->m_handle, 0, "eof-reached", MPV_FORMAT_FLAG);
    // scripts read the position every frame (IVideoTexture ended callbacks), a synchronous get would stall the same way
    mpv_observe_property (this->m_handle, POSITION_OBSERVER, "time-pos", MPV_FORMAT_DOUBLE);
    // a synchronous get of these blocked the render thread ~200ms while mpv's core thread set up the decoder
    mpv_observe_property (this->m_handle, WIDTH_OBSERVER, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property (this->m_handle, HEIGHT_OBSERVER, "dheight", MPV_FORMAT_INT64);

    const auto& hwdec = this->getContext ().getApp ().getContext ().settings.render.hwdec;

    if (mpv_set_property_string (this->m_handle, "hwdec", hwdec.c_str ()) < 0) {
	sLog.error ("mpv rejected hwdec value ", hwdec, ", decoding in software");
    }
    mpv_set_property_string (this->m_handle, "loop", this->m_loop ? "inf" : "no");
    mpv_set_property_string (this->m_handle, "keep-open", "yes");
    const auto [loopA, loopB] = abLoopPoints (this->m_segments);
    mpv_set_property_string (this->m_handle, "ab-loop-a", loopA.c_str ());
    mpv_set_property_string (this->m_handle, "ab-loop-b", loopB.c_str ());
    mpv_set_property (this->m_handle, "volume", MPV_FORMAT_DOUBLE, &this->m_volume);
    mpv_set_property (this->m_handle, "speed", MPV_FORMAT_DOUBLE, &this->m_speed);
    this->observePosition ();

    mpv_opengl_init_params gl_init_params { get_proc_address, this };
    // without the native display mpv can't import decoded frames into our GL context (VA-API dmabuf interop)
    // and falls back to a copy mode, every frame going GPU -> RAM -> GPU
    const auto& driver = this->getContext ().getDriver ();
    std::vector<mpv_render_param> params {
	{ MPV_RENDER_PARAM_API_TYPE, const_cast<char*> (MPV_RENDER_API_TYPE_OPENGL) },
	{ MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params },
    };

    if (void* display = driver.getWaylandDisplay ()) {
	params.push_back ({ MPV_RENDER_PARAM_WL_DISPLAY, display });
    } else if (void* display = driver.getX11Display ()) {
	params.push_back ({ MPV_RENDER_PARAM_X11_DISPLAY, display });
    }

    params.push_back ({ MPV_RENDER_PARAM_INVALID, nullptr });

    if (mpv_render_context_create (&this->m_renderContext, this->m_handle, params.data ()) < 0) {
	sLog.exception ("Failed to initialize MPV's GL context");
    }

    mpv_set_property_string (this->m_handle, "mute", this->m_muted ? "yes" : "no");
    this->m_enginePaused = this->m_paused || fixedStep () > 0.0;
    mpv_set_property_string (this->m_handle, "pause", this->m_enginePaused ? "yes" : "no");
}

void GLPlayer::setSource (const std::filesystem::path& file) { this->m_file = file; }

void GLPlayer::setSource (MemoryStreamProtocolUniquePtr source) { this->m_stream = std::move (source); }

void GLPlayer::play () {
    if (this->m_handle != nullptr) {
	sLog.exception ("Cannot play the same GLPlayer twice");
    }

    if (!this->m_file.has_value () && !this->m_stream.has_value ()) {
	sLog.exception ("Cannot play a GLPlayer without a source");
    }

    // sub_1400F2510 counts the texture as used when it's handed to the player
    this->m_lastUsedFrame = s_frame;
    this->init ();

    if (this->m_file.has_value ()) {
	const auto& path = this->m_file.value ();

	// another player showing the same video (mirrored monitor) sets the start position
	std::optional<double> start;

	if (!this->m_segments.empty ()) {
	    start = this->m_segments.front ().start;
	}

	if (const auto it = s_activePlayers.find (path); it != s_activePlayers.end ()) {
	    const double position = it->second->getPlaybackPosition ();
	    const bool inPart = this->m_segments.empty ()
		|| (position >= this->m_segments.front ().start && !this->skipTarget (position).has_value ()
		    && (!this->m_segments.back ().end.has_value () || position < *this->m_segments.back ().end));

	    if (position > 0.0 && inPart) {
		start = position;
	    }
	}

	// start at the first part. An option, not a loadfile argument: mpv 0.38 changed that form
	if (start.has_value ()) {
	    mpv_set_property_string (this->m_handle, "start", std::to_string (*start).c_str ());
	    this->m_position = *start;
	}

	const char* command[] = { "loadfile", path.c_str (), "replace", nullptr };

	if (mpv_command (this->m_handle, command) < 0) {
	    sLog.exception ("Cannot load video to play");
	}

	s_activePlayers[path] = this;
    } else if (this->m_stream) {
	this->m_stream.value ()->registerReadCallback (this->m_handle);

	const char* command[] = { "loadfile", "buffer://", nullptr };

	if (mpv_command (this->m_handle, command) < 0) {
	    sLog.exception ("Cannot load video texture to play");
	}
    }
}

void GLPlayer::stop () {
    // unregister, unless a newer player for the path took over
    if (this->m_file.has_value ()) {
	if (const auto it = s_activePlayers.find (this->m_file.value ());
	    it != s_activePlayers.end () && it->second == this) {
	    s_activePlayers.erase (it);
	}
    }

    if (this->m_renderContext) {
	mpv_render_context_free (this->m_renderContext);
	this->m_renderContext = nullptr;
    }

    if (this->m_handle) {
	mpv_terminate_destroy (this->m_handle);
	this->m_handle = nullptr;
    }
}