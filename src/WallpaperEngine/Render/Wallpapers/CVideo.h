#pragma once

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

namespace WallpaperEngine::Render::Wallpapers {
using namespace WallpaperEngine::VideoPlayback::MPV;

class CVideo final : public CWallpaper {
public:
    CVideo (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );

    ~CVideo () override;

    const Data::Model::Video& getVideo () const;

    [[nodiscard]] int getWidth () const override;
    [[nodiscard]] int getHeight () const override;

    void setPause (bool newState) override;

    /** Pushes a new volume (0-100, matching what GLPlayer expects) to the underlying mpv player without a reload */
    void setVolume (double volume);

    /** Pushes a new playback speed multiplier to the underlying mpv player without a reload, see --speed */
    void setSpeed (double speed);

    void setSegments (const VideoPlayback::VideoSegments& segments);

    /** Jumps to a position in seconds without changing the loop range */
    void seek (double seconds);

    /** Seconds from "90", "1:30" or "0:01:30.5", nullopt if the text is none of those */
    static std::optional<double> parseTime (const std::string& value);
    /** "2:00-3:00,4:00-5:00", empty sides are open, nullopt on a bad part */
    static std::optional<VideoPlayback::VideoSegments> parseSegments (const std::string& value);
    /** Sorted and merged, an open end swallows the rest, the whole video gives no segments */
    static VideoPlayback::VideoSegments normalizeSegments (VideoPlayback::VideoSegments segments);

    /** ambientVolume is ignored - video wallpapers always use --volume, only muted matters here */
    void setAudioPolicy (bool muted, std::optional<int> ambientVolume) override;

    /** WE 2.8 plays videos in its scene renderer (sub_140120050), --hdr has no WE counterpart */
    [[nodiscard]] bool drawsImageAdjustments () const override { return !this->m_hdr; }

protected:
    void renderFrame (const glm::ivec4& viewport) override;

    friend class CWallpaper;

private:
    void updateMuteState ();

    GLPlayerUniquePtr m_player;

    bool m_hdr = false;
    bool m_muted = false;
    /** Forced mute from --audio-screen (this screen isn't the designated audio screen) */
    bool m_forceMuted = false;
};
} // namespace WallpaperEngine::Render::Wallpapers
