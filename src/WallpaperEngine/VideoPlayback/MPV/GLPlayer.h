#pragma once

#include "MemoryStreamProtocol.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include <GL/glew.h>
#include <mpv/client.h>
#include <mpv/render.h>
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
    void clearUntimed ();
    void setMuted ();
    void clearMuted ();
    void setVolume (double volume);
    void setSpeed (double speed);
    void setPaused ();
    void clearPaused ();

    void render () const;

    int getWidth () const;
    int getHeight () const;

    /** Current playback position in seconds, or 0 if playback hasn't started yet */
    double getPlaybackPosition () const;

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
