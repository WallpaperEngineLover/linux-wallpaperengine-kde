#pragma once

#include "MemoryStreamProtocol.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include <GL/glew.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <optional>
#include <string>
#include <unordered_map>

namespace WallpaperEngine::VideoPlayback::MPV {
class GLPlayer : public Helpers::ContextAware {
    friend struct MemoryStreamProtocol;

public:
    GLPlayer (
	RenderContext& context, GLuint outputTexture, const std::filesystem::path& file, int64_t baseWidth,
	int64_t baseHeight, GLuint fbo = GL_NONE
    );
    GLPlayer (
	RenderContext& context, GLuint outputTexture, MemoryStreamProtocolUniquePtr stream, int64_t baseWidth,
	int64_t baseHeight, GLuint fbo = GL_NONE
    );
    ~GLPlayer () override;

    /** Refcounts playback: starts mpv on the first user */
    void incrementUsageCount ();
    /** Refcounts playback: stops mpv once the last user releases it */
    void decrementUsageCount ();

    void setUntimed ();
    /** For videos that are only ever a texture. Must be called before playback starts */
    void disableAudio ();
    /**
     * Decodes into a half float texture as linear light with BT.2020 primaries (1.0 = reference white) and no
     * tone mapping, so HDR videos keep their highlights for an HDR output. Must be called before playback starts
     */
    void setLinearOutput ();
    void clearUntimed ();
    void setMuted ();
    void clearMuted ();
    void setVolume (double volume);
    void setSpeed (double speed);
    [[nodiscard]] double getSpeed () const { return this->m_speed; }
    void setPaused ();
    void clearPaused ();
    void setLoop (bool loop);
    /** Jumps to a position in seconds, remembered and applied once the file has loaded if playback hasn't got that far */
    void seek (double seconds);
    /** Whether a non-looping video has played through to its end since the last seek */
    [[nodiscard]] bool hasEnded () const { return this->m_ended; }
    [[nodiscard]] bool isPaused () const { return this->m_paused; }
    [[nodiscard]] bool isLooping () const { return this->m_loop; }
    /** Total length in seconds, or 0 while it isn't known yet */
    double getDuration () const;

    void render () const;

    int getWidth () const;
    int getHeight () const;

    /** Current playback position in seconds, or 0 if playback hasn't started yet */
    double getPlaybackPosition () const;

    /** Time spent in render () since the last reset, only collected while LWE_FRAME_STATS is set */
    static double s_statsMillis;

private:
    void prepareGL ();
    void init ();
    void play ();
    void setSource (MemoryStreamProtocolUniquePtr source);
    void setSource (const std::filesystem::path& file);
    void stop ();

protected:
    bool m_doWeOwnFramebuffer;
    GLuint m_outputTexture;
    GLuint m_fbo = GL_NONE;
    mutable int64_t m_width;
    mutable int64_t m_height;
    mpv_handle* m_handle = nullptr;
    mpv_render_context* m_renderContext = nullptr;
    double m_volume = 0.0f;
    double m_speed = 1.0;
    bool m_muted = false;
    bool m_untimed = false;
    bool m_paused = false;
    bool m_loop = true;
    bool m_audio = true;
    bool m_linearOutput = false;
    // a texture we own only needs redrawing when mpv has a new frame, or after its size changed
    mutable bool m_needsRedraw = true;
    mutable bool m_fileLoaded = false;
    mutable bool m_ended = false;
    mutable std::optional<double> m_pendingSeek;
    std::optional<std::filesystem::path> m_file;
    std::optional<MemoryStreamProtocolUniquePtr> m_stream;
    uint32_t m_usageCount = 0;

    // tracks the currently playing instance for each video path, so a player starting
    // the same video another monitor is already showing can pick up around the same spot
    // instead of always restarting from zero
    static std::unordered_map<std::filesystem::path, GLPlayer*> s_activePlayers;
};

using GLPlayerUniquePtr = std::unique_ptr<GLPlayer>;
using GLPlayerSharedPtr = std::shared_ptr<GLPlayer>;
};
