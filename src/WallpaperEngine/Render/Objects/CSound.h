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

protected:
    void load ();

private:
    std::map<int, Audio::AudioStream*> m_audioStreams = {};

    const Sound& m_sound;
};
} // namespace WallpaperEngine::Render::Objects
