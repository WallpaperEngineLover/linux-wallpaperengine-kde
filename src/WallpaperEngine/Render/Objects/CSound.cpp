#include <SDL.h>

#include "CSound.h"

#include "WallpaperEngine/FileSystem/Container.h"

#include <algorithm>

using namespace WallpaperEngine::Render::Objects;

CSound::CSound (Wallpapers::CScene& scene, const Sound& sound) : CObject (scene, sound), m_sound (sound) {
    if (this->getContext ().getApp ().getContext ().settings.audio.enabled) {
	this->load ();
    }
}

CSound::~CSound () {
    for (const auto& stream : this->m_audioStreams) {
	this->getScene ().getAudioContext ().removeStream (stream.first);
	delete stream.second;
    }

    this->m_audioStreams.clear ();
}

void CSound::load () {
    for (const auto& cur : this->m_sound.sounds) {
	auto stream
	    = new Audio::AudioStream (this->getScene ().getAudioContext (), this->getAssetLocator ().read (cur));

	stream->setRepeat (this->m_sound.playbackmode.has_value () && this->m_sound.playbackmode == "loop");

	this->m_audioStreams.insert_or_assign (this->getScene ().getAudioContext ().addStream (stream), stream);
    }
}

void CSound::render () { this->applyEffectiveVolume (); }

void CSound::setVolumeOverride (std::optional<int> volume) {
    this->m_screenVolumeOverride = volume;
    this->applyEffectiveVolume ();
}

void CSound::applyEffectiveVolume () {
    // The screen-level policy (mute/ambient-volume, set via setVolumeOverride from
    // CScene::setAudioPolicy) and the wallpaper author's own per-object "volume" property (e.g.
    // picking which of several alternate music tracks plays) are independent inputs that have to
    // combine, not overwrite each other - otherwise picking a track would undo screen muting, or
    // muting a screen would make track selection pointless.
    const int base = this->m_screenVolumeOverride.value_or (
	this->getContext ().getApp ().getContext ().state.audio.volume
    );
    const float fraction = this->m_sound.volume && this->m_sound.volume->value
	? std::clamp (this->m_sound.volume->value->getFloat (), 0.0f, 1.0f)
	: 1.0f;
    const int effective = static_cast<int> (static_cast<float> (base) * fraction);

    for (const auto& entry : this->m_audioStreams) {
	this->getScene ().getAudioContext ().setStreamVolume (entry.first, effective);
    }
}
