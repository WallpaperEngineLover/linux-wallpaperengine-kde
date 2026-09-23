#include "GLPlayer.h"

#include "WallpaperEngine/Logging/Log.h"

#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace WallpaperEngine::VideoPlayback::MPV;

std::unordered_map<std::filesystem::path, GLPlayer*> GLPlayer::s_activePlayers;
double GLPlayer::s_statsMillis = 0.0;

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
}

GLPlayer::GLPlayer (
    RenderContext& context, const GLuint outputTexture, MemoryStreamProtocolUniquePtr stream, const int64_t baseWidth,
    const int64_t baseHeight, const GLuint fbo
) : ContextAware (context), m_outputTexture (outputTexture), m_width (baseWidth), m_height (baseHeight) {
    this->m_fbo = fbo;
    this->m_doWeOwnFramebuffer = this->m_fbo == GL_NONE;

    this->prepareGL ();
    this->setSource (std::move (stream));
}

GLPlayer::~GLPlayer () {
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

void GLPlayer::setPaused () {
    this->m_paused = true;

    if (this->m_handle) {
	setPropertyAsync (this->m_handle, "pause", "yes");
    }
}

void GLPlayer::clearPaused () {
    this->m_paused = false;

    if (this->m_handle) {
	setPropertyAsync (this->m_handle, "pause", "no");
    }
}

void GLPlayer::setLoop (bool loop) {
    this->m_loop = loop;

    if (this->m_handle) {
	setPropertyAsync (this->m_handle, "loop", loop ? "inf" : "no");
    }
}

void GLPlayer::seek (const double seconds) {
    this->m_ended = false;

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

void GLPlayer::render () const {
    // only render while actively playing (m_handle is set by usage-count-driven play())
    if (this->m_handle == nullptr) {
	return;
    }

    while (true) {
	const mpv_event* event = mpv_wait_event (this->m_handle, 0);

	if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
	    break;
	}

	if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
	    const auto* property = static_cast<const mpv_event_property*> (event->data);

	    if (property->format == MPV_FORMAT_FLAG && std::string_view (property->name) == "eof-reached"
		&& *static_cast<const int*> (property->data) && !this->m_loop) {
		this->m_ended = true;
	    }

	    continue;
	}

	if (event->event_id == MPV_EVENT_FILE_LOADED) {
	    this->m_fileLoaded = true;

	    if (this->m_pendingSeek.has_value ()) {
		const std::string target = std::to_string (*this->m_pendingSeek);
		const char* command[] = { "seek", target.c_str (), "absolute+exact", nullptr };

		this->m_pendingSeek.reset ();
		mpv_command_async (this->m_handle, 0, command);
	    }

	    continue;
	}

	if (event->event_id != MPV_EVENT_VIDEO_RECONFIG) {
	    continue;
	}

	int64_t width, height;

	if (mpv_get_property (this->m_handle, "dwidth", MPV_FORMAT_INT64, &width) < 0) {
	    continue;
	}

	if (mpv_get_property (this->m_handle, "dheight", MPV_FORMAT_INT64, &height) < 0) {
	    continue;
	}

	if (width < 0 || height < 0) {
	    continue;
	}

	this->m_width = width;
	this->m_height = height;
	this->m_needsRedraw = true;
	glBindTexture (GL_TEXTURE_2D, this->m_outputTexture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, this->m_width, this->m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }

    // mpv only hands out its next frame once the update flags were collected, without this playback stalls
    const uint64_t updateFlags = mpv_render_context_update (this->m_renderContext);

    if (this->m_doWeOwnFramebuffer) {
	if (!(updateFlags & MPV_RENDER_UPDATE_FRAME) && !this->m_needsRedraw) {
	    return;
	}

	this->m_needsRedraw = false;
    }

    glViewport (0, 0, this->m_width, this->m_height);

    mpv_opengl_fbo fbo { static_cast<int> (this->m_fbo), static_cast<int> (this->m_width),
			 static_cast<int> (this->m_height), GL_RGBA8 };

    // no need to flip as it'll be handled by the wallpaper rendering code
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
	return;
    }

    glFinish ();
    const auto start = std::chrono::steady_clock::now ();
    mpv_render_context_render (this->m_renderContext, params);
    glFinish ();
    s_statsMillis += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
}

int GLPlayer::getWidth () const { return this->m_width; }
int GLPlayer::getHeight () const { return this->m_height; }

double GLPlayer::getPlaybackPosition () const {
    double position = 0.0;

    if (this->m_handle != nullptr) {
	mpv_get_property (this->m_handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
    }

    return position;
}

void GLPlayer::prepareGL () {
    if (!this->m_doWeOwnFramebuffer || this->m_fbo != GL_NONE) {
	return;
    }

    glGenFramebuffers (1, &this->m_fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_fbo);
    glBindTexture (GL_TEXTURE_2D, this->m_outputTexture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, this->m_width, this->m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    constexpr GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_outputTexture, 0);
    glDrawBuffers (1, drawBuffers);

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.exception ("Framebuffers are not properly set");
    }

    glClear (GL_COLOR_BUFFER_BIT);
}

void GLPlayer::init () {
    this->m_fileLoaded = false;
    this->m_ended = false;
    this->m_handle = mpv_create ();

    if (this->m_handle == nullptr) {
	sLog.exception ("Cannot create mpv context for video texture");
    }

    // setup mpv options for playback
    mpv_set_option_string (this->m_handle, "terminal", "yes");
#if NDEBUG
    mpv_set_option_string (this->m_handle, "msg-level", "all=status,statusline=no");
#else
    mpv_set_option_string (this->m_handle, "msg-level", "all=v");
#endif
    mpv_set_option_string (this->m_handle, "input-cursor", "no");
    mpv_set_option_string (this->m_handle, "cursor-autohide", "no");
    mpv_set_option_string (this->m_handle, "config", "no");
    mpv_set_option_string (this->m_handle, "fbo-format", "rgba8");
    mpv_set_option_string (this->m_handle, "vo", "libmpv");
    mpv_set_option_string (this->m_handle, "profile", "fast");
    mpv_set_option_string (this->m_handle, "untimed", this->m_untimed ? "yes" : "no");
    if (!this->m_audio) {
	mpv_set_option_string (this->m_handle, "aid", "no");
    }
    if (this->m_doWeOwnFramebuffer) {
	// without the blocking wait in render() a frame would otherwise show up to 50ms early
	mpv_set_option_string (this->m_handle, "video-timing-offset", "0");
    }

    if (mpv_initialize (this->m_handle) < 0) {
	sLog.exception ("Could not initialize mpv context");
    }

    // keep-open parks a finished video on its last frame instead of unloading it, so a script can still seek it
    // back to the start. Observed rather than polled: mpv_get_property waits for mpv's core thread, and polling
    // it every frame stalled the render thread ~160-200ms every few seconds on a paused video
    mpv_observe_property (this->m_handle, 0, "eof-reached", MPV_FORMAT_FLAG);

    mpv_set_property_string (this->m_handle, "hwdec", "auto");
    mpv_set_property_string (this->m_handle, "loop", this->m_loop ? "inf" : "no");
    mpv_set_property_string (this->m_handle, "keep-open", "yes");
    mpv_set_property (this->m_handle, "volume", MPV_FORMAT_DOUBLE, &this->m_volume);
    mpv_set_property (this->m_handle, "speed", MPV_FORMAT_DOUBLE, &this->m_speed);

    // initialize gl context for mpv
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
    mpv_set_property_string (this->m_handle, "pause", this->m_paused ? "yes" : "no");
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

    this->init ();

    if (this->m_file.has_value ()) {
	const auto& path = this->m_file.value ();

	// if another player is already showing the same video (e.g. mirrored on another
	// monitor), start around the same position instead of always restarting from zero
	std::string startOption;
	std::vector<const char*> command = { "loadfile", path.c_str (), "replace" };

	if (const auto it = s_activePlayers.find (path); it != s_activePlayers.end ()) {
	    if (const double position = it->second->getPlaybackPosition (); position > 0.0) {
		startOption = "start=" + std::to_string (position);
		command.push_back (startOption.c_str ());
	    }
	}

	command.push_back (nullptr);

	if (mpv_command (this->m_handle, command.data ()) < 0) {
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
    // drop ourselves from the active players list, but only if we're still the one registered
    // (a newer player for the same path may have already taken over the slot)
    if (this->m_file.has_value ()) {
	if (const auto it = s_activePlayers.find (this->m_file.value ()); it != s_activePlayers.end () && it->second == this) {
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