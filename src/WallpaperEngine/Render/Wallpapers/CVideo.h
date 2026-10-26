#pragma once

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include <filesystem>
#include <unordered_map>
#include <vector>

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

    // 0-100 like GLPlayer, live
    void setVolume (double volume);

    void setSpeed (double speed);

    /** a playlist with videosequence plays its videos to the end instead of looping them (wallpaper64 sub_140076ED0) */
    void setLoop (bool loop);
    [[nodiscard]] bool hasEnded () const;

    void setSegments (const VideoPlayback::VideoSegments& segments);

    void seek (double seconds);

    static std::optional<double> parseTime (const std::string& value);
    static std::optional<VideoPlayback::VideoSegments> parseSegments (const std::string& value);
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
    CVideo& root () { return this->m_source != nullptr ? *this->m_source : *this; }
    [[nodiscard]] const CVideo& root () const { return this->m_source != nullptr ? *this->m_source : *this; }
    void drivePlayer ();
    void unshare ();
    void createPlayer ();
    void applyPause ();
    void reconcileSharers ();
    void copyFromSource ();

    GLPlayerUniquePtr m_player;
    std::filesystem::path m_path;
    CVideo* m_source = nullptr;
    std::vector<CVideo*> m_sharers;
    bool m_loop = true;
    bool m_wantsPause = false;
    VideoPlayback::VideoSegments m_segments;

    static std::unordered_map<std::filesystem::path, CVideo*> s_sources;

    bool m_hdr = false;
    bool m_muted = false;
    /** Forced mute from --audio-screen (this screen isn't the designated audio screen) */
    bool m_forceMuted = false;
};
}
