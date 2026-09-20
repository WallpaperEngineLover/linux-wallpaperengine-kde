#pragma once

#include <optional>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Render/CObject.h"

using namespace WallpaperEngine;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

class CSound final : public CObject {
public:
    CSound (Wallpapers::CScene& scene, const Sound& sound);
    ~CSound () override;

    void render () override;

    /** Overrides the volume (0-128) this sound's streams mix at instead of the global volume; nullopt = global */
    void setVolumeOverride (std::optional<int> volume);

    /** Script-facing playback control (thisScene.getLayer(<sound name>).play()/stop()/pause()) */
    void play ();
    void stop ();
    [[nodiscard]] bool isPlaying () const { return this->m_playing; }

protected:
    void load ();

private:
    void applyEffectiveVolume ();

    std::map<int, Audio::AudioStream*> m_audioStreams = {};

    const Sound& m_sound;
    /** Screen-level mute/ambient-volume policy from CScene::setAudioPolicy; nullopt = no override */
    std::optional<int> m_screenVolumeOverride;
    bool m_playing = true;
};
} // namespace WallpaperEngine::Render::Objects
