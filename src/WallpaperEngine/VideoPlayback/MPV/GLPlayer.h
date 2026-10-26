#pragma once

#include "MemoryStreamProtocol.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/VideoPlayback/VideoSegment.h"

#include <GL/glew.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

    void incrementUsageCount ();
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
    void setSegments (const VideoSegments& segments);
    void seek (double seconds);
    [[nodiscard]] bool hasEnded () const { return this->m_ended; }
    [[nodiscard]] bool isPaused () const { return this->m_paused; }
    [[nodiscard]] bool isLooping () const { return this->m_loop; }
    double getDuration () const;

    void render () const;
    void requestRedraw () const { this->m_needsRedraw = true; }
    /** several outputs drive this player: a call that finds no new frame must not draw again, nor wait for one */
    void setRenderOnlyNew (bool value) { this->m_renderOnlyNew = value; }
    /** The texture was bound for a draw this frame, keeps playback running (WE's transfer on bind, sub_14015F0D0) */
    void markUsed () const;
    /**
     * Once per scene frame, like the counter wallpaper64.exe 2.8.42 sub_14017FA70 bumps. A video whose texture went
     * unused for 30 of them is paused until it's drawn again (sub_1400F3930)
     */
    static void advanceFrame ();
    static void setGlobalPause (bool paused);

    int getWidth () const;
    int getHeight () const;

    double getPlaybackPosition () const { return this->m_position; }

    /** Comma separated backend names only, so no other mpv option gets injected */
    static bool isValidHwdec (const std::string& value);
    static void setHwdecForAll (const std::string& value);

    static double s_statsMillis;

private:
    void prepareGL ();
    /** to transparent black, the texture must be one we own */
    void clearTexture () const;
    /** mpv keeps seeking back to an A past the end, so those parts are dropped once the length is known */
    bool dropSegmentsPastEnd () const;
    void observePosition () const;
    std::optional<double> skipTarget (double position) const;
    void init ();
    void applyPause () const;
    void play ();
    void setSource (MemoryStreamProtocolUniquePtr source);
    void setSource (const std::filesystem::path& file);
    void stop ();

protected:
    void applyDisplaySize () const;
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
    mutable bool m_enginePaused = false;
    mutable uint64_t m_lastUsedFrame = 0;
    mutable double m_position = 0.0;
    mutable int64_t m_displayWidth = 0;
    mutable int64_t m_displayHeight = 0;
    bool m_loop = true;
    bool m_audio = true;
    bool m_linearOutput = false;
    bool m_renderOnlyNew = false;
    // a texture we own only needs redrawing when mpv has a new frame, or after its size changed
    mutable bool m_needsRedraw = true;
    mutable bool m_fileLoaded = false;
    /** mpv drew a decoded frame since init, before that its redraws are an empty black picture */
    mutable bool m_hasFrame = false;
    mutable bool m_ended = false;
    mutable std::optional<double> m_pendingSeek;
    mutable VideoSegments m_segments;
    /** positions reported until it lands are stale */
    mutable bool m_skipping = false;
    mutable double m_frameDuration = 0.0;
    std::optional<std::filesystem::path> m_file;
    std::optional<MemoryStreamProtocolUniquePtr> m_stream;
    uint32_t m_usageCount = 0;

    // playing instance per path, so a mirrored video starts near the same spot
    static std::unordered_map<std::filesystem::path, GLPlayer*> s_activePlayers;
    static std::unordered_set<GLPlayer*> s_players;
    static uint64_t s_frame;
    static bool s_globalPause;
};

using GLPlayerUniquePtr = std::unique_ptr<GLPlayer>;
using GLPlayerSharedPtr = std::shared_ptr<GLPlayer>;
};
